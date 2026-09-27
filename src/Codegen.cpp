#include "Codegen.hpp"

/* LLVM deprecates some stable APIs (legacy PassManager, CreateGlobalString, …).
   Suppress until we migrate fully to the new pass manager / IRBuilder helpers. */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/DebugInfoMetadata.h>

#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/Config/llvm-config.h>

#include <cstdio>
#include <cstdlib>
#include <system_error>

using namespace llvm;

Codegen::Codegen(const std::string& moduleName, const std::string& sourcePath) {
    ctx_ = new LLVMContext();
    module_ = new Module(moduleName, *ctx_);
    builder_ = new IRBuilder<>(*ctx_);
    sourcePath_ = sourcePath.empty() ? "input.simple" : sourcePath;
    emitDebug_ = true;

    // DWARF debug info
    module_->addModuleFlag(Module::Warning, "Debug Info Version", DEBUG_METADATA_VERSION);
    module_->addModuleFlag(Module::Warning, "Dwarf Version", 5);
    dbg_ = new DIBuilder(*module_);
    file_ = dbg_->createFile(sourcePath_, ".");
    cu_ = dbg_->createCompileUnit(
        dwarf::DW_LANG_C, file_, "Simple Compiler", false, "", 0);
}

Codegen::~Codegen() {
    if (dbg_) {
        delete dbg_;
        dbg_ = nullptr;
    }
    delete builder_;
    delete module_;
    delete ctx_;
}

void Codegen::setDebugLoc(int line, int col) {
    if (!emitDebug_ || !dbg_ || !currentSP_) return;
    if (line <= 0) line = 1;
    if (col <= 0) col = 1;
    builder_->SetCurrentDebugLocation(
        DILocation::get(*ctx_, (unsigned)line, (unsigned)col, currentSP_));
}

DIType* Codegen::diBasicType(const char* name, unsigned bits, unsigned encoding) {
    return dbg_->createBasicType(name, bits, encoding);
}

DISubprogram* Codegen::createDIFunction(Function* f, const std::string& name,
                                        int line, bool isLocal) {
    if (!emitDebug_ || !dbg_ || !file_) return nullptr;
    DIType* retTy = diBasicType("int", 64, dwarf::DW_ATE_signed);
    SmallVector<Metadata*, 8> elts;
    elts.push_back(retTy);
    DITypeRefArray paramTypes = dbg_->getOrCreateTypeArray(elts);
    DISubroutineType* fty = dbg_->createSubroutineType(paramTypes);
    DISubprogram* sp = dbg_->createFunction(
        file_, name, name, file_,
        line > 0 ? (unsigned)line : 1,
        fty, line > 0 ? (unsigned)line : 1,
        DINode::FlagPrototyped,
        DISubprogram::SPFlagDefinition);
    f->setSubprogram(sp);
    return sp;
}

Type* Codegen::mapType(AstNode* ts) {
    if (!ts || ts->kind != AstKind::TypeSpec)
        return Type::getVoidTy(*ctx_);
    if (ts->isLiteralType) {
        switch (ts->typeTok) {
            case TokenKind::Integer: case TokenKind::Int:
            case TokenKind::Unsigned:
                return Type::getInt64Ty(*ctx_);
            case TokenKind::Float:
                return Type::getDoubleTy(*ctx_);
            case TokenKind::Boolean: case TokenKind::Bool:
                return Type::getInt1Ty(*ctx_);
            case TokenKind::String:
            case TokenKind::List:
            case TokenKind::Dict:
                return PointerType::getUnqual(*ctx_);
            case TokenKind::Nothing:
                return Type::getVoidTy(*ctx_);
            case TokenKind::Null:
                return PointerType::getUnqual(*ctx_);
            default:
                return PointerType::getUnqual(*ctx_);
        }
    }
    if (ts->left && ts->left->kind == AstKind::CompoundName && !ts->left->nameParts.empty()) {
        const std::string& cn = ts->left->nameParts.back();
        auto it = classes_.find(cn);
        if (it != classes_.end())
            return PointerType::getUnqual(it->second);
    }
    return PointerType::getUnqual(*ctx_);
}

AllocaInst* Codegen::createEntryAlloca(Function* fn, Type* ty, const std::string& name) {
    IRBuilder<> tmp(&fn->getEntryBlock(), fn->getEntryBlock().begin());
    return tmp.CreateAlloca(ty, nullptr, name);
}

Value* Codegen::emitExpr(AstNode* e) {
    if (!e) return ConstantInt::get(Type::getInt64Ty(*ctx_), 0);

    switch (e->kind) {
        case AstKind::Literal: {
            switch (e->litKind) {
                case TokenKind::LitInt:
                case TokenKind::LitUns: {
                    long long v = std::strtoll(e->text.c_str(), nullptr, 10);
                    return ConstantInt::get(Type::getInt64Ty(*ctx_), v, true);
                }
                case TokenKind::LitFloat: {
                    double v = std::strtod(e->text.c_str(), nullptr);
                    return ConstantFP::get(Type::getDoubleTy(*ctx_), v);
                }
                case TokenKind::LitBool: {
                    bool v = (e->text == "true");
                    return ConstantInt::get(Type::getInt1Ty(*ctx_), v);
                }
                case TokenKind::Null:
                    return ConstantPointerNull::get(PointerType::getUnqual(*ctx_));
                case TokenKind::LitSStr:
                case TokenKind::LitDStr: {
                    std::string s = e->text;
                    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\''))
                        s = s.substr(1, s.size() - 2);
                    return builder_->CreateGlobalString(s, "str");
                }
                default:
                    return ConstantInt::get(Type::getInt64Ty(*ctx_), 0);
            }
        }
        case AstKind::Identifier: {
            if (inGenerator_) {
                auto gs = genSlots_.find(e->name);
                if (gs != genSlots_.end())
                    return genGetInt(gs->second);
            }
            auto it = locals_.find(e->name);
            if (it != locals_.end())
                return builder_->CreateLoad(it->second->getAllocatedType(), it->second, e->name);
            // Field via self
            if (inMethod_ && !currentClass_.empty()) {
                auto selfIt = locals_.find("self");
                if (selfIt != locals_.end()) {
                    auto clsIt = classes_.find(currentClass_);
                    if (clsIt != classes_.end()) {
                        Value* self = builder_->CreateLoad(
                            PointerType::getUnqual(clsIt->second), selfIt->second, "self.ptr");
                        unsigned idx = 1;
                        auto fi = fieldIndex_.find(currentClass_);
                        if (fi != fieldIndex_.end()) {
                            auto it2 = fi->second.find(e->name);
                            if (it2 != fi->second.end()) idx = it2->second;
                        }
                        emitAccessCheck(currentClass_, e->name);
                        Value* gep = builder_->CreateStructGEP(clsIt->second, self, idx, e->name + ".ptr");
                        Type* fieldTy = clsIt->second->getElementType(idx);
                        return builder_->CreateLoad(fieldTy, gep, e->name);
                    }
                }
            }
            return ConstantInt::get(Type::getInt64Ty(*ctx_), 0);
        }
        case AstKind::BinaryOp: {
            Value* l = emitExpr(e->left.get());
            Value* r = emitExpr(e->right.get());
            auto unboxIf = [&](Value* v) -> Value* {
                if (!v || !v->getType()->isPointerTy()) return v;
                Type* i8p = PointerType::getUnqual(*ctx_);
                FunctionCallee isbox = module_->getOrInsertFunction(
                    "s_is_int_box", FunctionType::get(Type::getInt32Ty(*ctx_), {i8p}, false));
                FunctionCallee unboxI = module_->getOrInsertFunction(
                    "s_unbox_int", FunctionType::get(Type::getInt64Ty(*ctx_), {i8p}, false));
                Value* ib = builder_->CreateCall(isbox, {v});
                Value* isI = builder_->CreateICmpNE(ib, ConstantInt::get(Type::getInt32Ty(*ctx_), 0));
                Value* asInt = builder_->CreateCall(unboxI, {v});
                Value* zero = ConstantInt::get(Type::getInt64Ty(*ctx_), 0);
                return builder_->CreateSelect(isI, asInt, zero, "unbox");
            };
            if (e->op != TokenKind::Is &&
                ((l && l->getType()->isPointerTy()) || (r && r->getType()->isPointerTy())) &&
                e->op != TokenKind::Eq && e->op != TokenKind::Equ &&
                e->op != TokenKind::Ne && e->op != TokenKind::Neq) {
                if (l && l->getType()->isPointerTy()) l = unboxIf(l);
                if (r && r->getType()->isPointerTy()) r = unboxIf(r);
            }
            switch (e->op) {
                case TokenKind::Is: {
                    Type* i8p = PointerType::getUnqual(*ctx_);
                    FunctionCallee isfn = module_->getOrInsertFunction(
                        "s_is_classname",
                        FunctionType::get(Type::getInt1Ty(*ctx_), {i8p, i8p}, false));
                    std::string cn = "object";
                    if (e->right && e->right->kind == AstKind::CompoundName && !e->right->nameParts.empty())
                        cn = e->right->nameParts.back();
                    Value* nm = builder_->CreateGlobalString(cn, "isname");
                    return builder_->CreateCall(isfn, {l, nm}, "is");
                }
                case TokenKind::Plus:
                    if (l->getType()->isFloatingPointTy() || r->getType()->isFloatingPointTy()) {
                        if (!l->getType()->isDoubleTy()) l = builder_->CreateSIToFP(l, Type::getDoubleTy(*ctx_));
                        if (!r->getType()->isDoubleTy()) r = builder_->CreateSIToFP(r, Type::getDoubleTy(*ctx_));
                        return builder_->CreateFAdd(l, r, "fadd");
                    }
                    return builder_->CreateAdd(l, r, "add");
                case TokenKind::Minus:
                    if (l->getType()->isFloatingPointTy() || r->getType()->isFloatingPointTy()) {
                        if (!l->getType()->isDoubleTy()) l = builder_->CreateSIToFP(l, Type::getDoubleTy(*ctx_));
                        if (!r->getType()->isDoubleTy()) r = builder_->CreateSIToFP(r, Type::getDoubleTy(*ctx_));
                        return builder_->CreateFSub(l, r, "fsub");
                    }
                    return builder_->CreateSub(l, r, "sub");
                case TokenKind::Star:
                    if (l->getType()->isFloatingPointTy() || r->getType()->isFloatingPointTy()) {
                        if (!l->getType()->isDoubleTy()) l = builder_->CreateSIToFP(l, Type::getDoubleTy(*ctx_));
                        if (!r->getType()->isDoubleTy()) r = builder_->CreateSIToFP(r, Type::getDoubleTy(*ctx_));
                        return builder_->CreateFMul(l, r, "fmul");
                    }
                    return builder_->CreateMul(l, r, "mul");
                case TokenKind::Slash:
                    if (l->getType()->isFloatingPointTy() || r->getType()->isFloatingPointTy()) {
                        if (!l->getType()->isDoubleTy()) l = builder_->CreateSIToFP(l, Type::getDoubleTy(*ctx_));
                        if (!r->getType()->isDoubleTy()) r = builder_->CreateSIToFP(r, Type::getDoubleTy(*ctx_));
                        return builder_->CreateFDiv(l, r, "fdiv");
                    }
                    return builder_->CreateSDiv(l, r, "div");
                case TokenKind::Percent: return builder_->CreateSRem(l, r, "rem");
                case TokenKind::Eq: case TokenKind::Equ:
                    return builder_->CreateICmpEQ(l, r, "eq");
                case TokenKind::Ne: case TokenKind::Neq:
                    return builder_->CreateICmpNE(l, r, "ne");
                case TokenKind::LtOp: case TokenKind::Lt:
                    return builder_->CreateICmpSLT(l, r, "lt");
                case TokenKind::GtOp: case TokenKind::Gt:
                    return builder_->CreateICmpSGT(l, r, "gt");
                case TokenKind::Le: case TokenKind::Lte:
                    return builder_->CreateICmpSLE(l, r, "le");
                case TokenKind::Ge: case TokenKind::Gte:
                    return builder_->CreateICmpSGE(l, r, "ge");
                case TokenKind::AndAnd: case TokenKind::And:
                    return builder_->CreateAnd(l, r, "and");
                case TokenKind::OrOr: case TokenKind::Or:
                    return builder_->CreateOr(l, r, "or");
                default:
                    return l;
            }
        }
        case AstKind::UnaryOp: {
            Value* v = emitExpr(e->left.get());
            if (e->op == TokenKind::Copy) {
                std::string cn = e->className;
                if (cn.empty()) cn = currentClass_;
                Function* cf = cn.empty() ? nullptr : module_->getFunction(cn + "_copy");
                if (!cf) {
                    for (auto& kv : classes_) {
                        cf = module_->getFunction(kv.first + "_copy");
                        if (cf) break;
                    }
                }
                if (cf) return builder_->CreateCall(cf, {v}, "copy");
                return v;
            }
            if (e->op == TokenKind::Minus) return builder_->CreateNeg(v, "neg");
            return builder_->CreateNot(v, "not");
        }
        case AstKind::CompoundRef: {
            if (e->children.empty()) return ConstantInt::get(Type::getInt64Ty(*ctx_), 0);
            if (e->children.size() == 1) {
                if (e->children[0]->kind == AstKind::Identifier)
                    return emitExpr(e->children[0].get());
                if (e->children[0]->kind == AstKind::FunctionCall) {
                    auto& call = e->children[0];
                    Function* fn = module_->getFunction(call->name);
                    if (!fn) return ConstantInt::get(Type::getInt64Ty(*ctx_), 0);
                    std::vector<Value*> args;
                    for (auto& a : call->args) args.push_back(emitExpr(a.get()));
                    return builder_->CreateCall(fn, args);
                }
            }
            if (e->children[0]->kind != AstKind::Identifier)
                return ConstantInt::get(Type::getInt64Ty(*ctx_), 0);
            std::string head = e->children[0]->name;
            /* Class.create(...) factory */
            if (e->children.size() >= 2 && e->children[1]->kind == AstKind::FunctionCall &&
                e->children[1]->name == "create" && classes_.count(head)) {
                Function* ctor = module_->getFunction(head + "_create");
                if (!ctor) return Constant::getNullValue(PointerType::getUnqual(*ctx_));
                std::vector<Value*> args;
                for (auto& a : e->children[1]->args) args.push_back(emitExpr(a.get()));
                Value* obj = builder_->CreateCall(ctor, args, "newobj");
                for (size_t i = 2; i < e->children.size(); ++i) {
                    auto& item = e->children[i];
                    if (item->kind == AstKind::FunctionCall) {
                        Function* mf = module_->getFunction(head + "_" + item->name);
                        if (mf) {
                            std::vector<Value*> ma; ma.push_back(obj);
                            for (auto& a : item->args) ma.push_back(emitExpr(a.get()));
                            obj = builder_->CreateCall(mf, ma);
                        }
                    }
                }
                return obj;
            }
            /* namespace.fn(...) from import as Alias */
            if (e->children.size() >= 2 && e->children[1]->kind == AstKind::FunctionCall &&
                locals_.find(head) == locals_.end() && !classes_.count(head)) {
                auto& call = e->children[1];
                Function* fn = module_->getFunction(call->name);
                if (!fn) fn = module_->getFunction(head + "_" + call->name);
                if (fn) {
                    std::vector<Value*> args;
                    for (auto& a : call->args) args.push_back(emitExpr(a.get()));
                    return builder_->CreateCall(fn, args);
                }
            }
            Value* obj = emitExpr(e->children[0].get());
            for (size_t i = 1; i < e->children.size(); ++i) {
                auto& item = e->children[i];
                if (item->kind == AstKind::FunctionCall) {
                    std::string stcls;
                    if (localClass_.count(head)) stcls = localClass_[head];
                    if (stcls.empty()) stcls = currentClass_;
                    if (!stcls.empty() && virtIndex_.count(stcls) &&
                        virtIndex_[stcls].count(item->name)) {
                        unsigned slot = virtIndex_[stcls][item->name];
                        Type* i8p = PointerType::getUnqual(*ctx_);
                        Value* raw = obj;
                        if (!raw->getType()->isPointerTy())
                            raw = builder_->CreateIntToPtr(raw, i8p);
                        Value* vpp = builder_->CreateBitCast(raw, PointerType::getUnqual(i8p), "vpp");
                        Value* vt = builder_->CreateLoad(i8p, vpp, "vt");
                        Value* vta = builder_->CreateBitCast(vt, PointerType::getUnqual(i8p), "vta");
                        Value* slotp = builder_->CreateGEP(i8p, vta,
                            ConstantInt::get(Type::getInt32Ty(*ctx_), slot), "vslot");
                        Value* fptr = builder_->CreateLoad(i8p, slotp, "vfn");
                        std::vector<Value*> args;
                        args.push_back(raw);
                        for (auto& a : item->args) args.push_back(emitExpr(a.get()));
                        Function* proto = module_->getFunction(stcls + "_" + item->name);
                        Type* ret = proto ? proto->getReturnType() : Type::getVoidTy(*ctx_);
                        std::vector<Type*> pts;
                        for (auto* a : args) pts.push_back(a->getType());
                        FunctionType* ft = FunctionType::get(ret, pts, false);
                        if (ret->isVoidTy()) {
                            builder_->CreateCall(ft, fptr, args);
                        } else {
                            obj = builder_->CreateCall(ft, fptr, args);
                        }
                        continue;
                    }
                    Function* fn = nullptr;
                    if (!currentClass_.empty())
                        fn = module_->getFunction(currentClass_ + "_" + item->name);
                    if (!fn)
                        fn = module_->getFunction(head + "_" + item->name);
                    if (!fn) {
                        for (auto& kv : classes_) {
                            fn = module_->getFunction(kv.first + "_" + item->name);
                            if (fn) break;
                        }
                    }
                    if (fn) {
                        std::string owner = fn->getName().str();
                        auto us = owner.find('_');
                        if (us != std::string::npos) owner = owner.substr(0, us);
                        emitAccessCheck(owner, item->name);
                        std::vector<Value*> args;
                        args.push_back(obj);
                        for (auto& a : item->args) args.push_back(emitExpr(a.get()));
                        if (fn->getReturnType()->isVoidTy()) {
                            builder_->CreateCall(fn, args);
                        } else {
                            obj = builder_->CreateCall(fn, args);
                        }
                    }
                } else if (item->kind == AstKind::Identifier) {
                    /* field load: search fieldIndex_ */
                    unsigned idx = 0;
                    StructType* sty = nullptr;
                    std::string owner;
                    for (auto& kv : fieldIndex_) {
                        auto fit = kv.second.find(item->name);
                        if (fit != kv.second.end()) {
                            idx = fit->second;
                            owner = kv.first;
                            auto cit = classes_.find(kv.first);
                            if (cit != classes_.end()) sty = cit->second;
                            break;
                        }
                    }
                    if (sty && idx > 0) {
                        emitAccessCheck(owner, item->name);
                        Value* gep = builder_->CreateStructGEP(sty, obj, idx, item->name);
                        obj = builder_->CreateLoad(sty->getElementType(idx), gep, item->name);
                    }
                }
            }
            return obj;
        }
        case AstKind::ArrayLiteral: {
            Type* i8p = PointerType::getUnqual(*ctx_);
            FunctionCallee create = module_->getOrInsertFunction(
                "s_list_create", FunctionType::get(i8p, {}, false));
            FunctionCallee push = module_->getOrInsertFunction(
                "s_list_push", FunctionType::get(Type::getVoidTy(*ctx_), {i8p, i8p}, false));
            FunctionCallee boxI = module_->getOrInsertFunction(
                "s_box_int", FunctionType::get(i8p, {Type::getInt64Ty(*ctx_)}, false));
            FunctionCallee boxF = module_->getOrInsertFunction(
                "s_box_float", FunctionType::get(i8p, {Type::getDoubleTy(*ctx_)}, false));
            Value* lst = builder_->CreateCall(create, {}, "list");
            for (auto& el : e->children) {
                Value* v = emitExpr(el.get());
                Value* boxed = v;
                if (v->getType()->isIntegerTy(64))
                    boxed = builder_->CreateCall(boxI, {v});
                else if (v->getType()->isIntegerTy(1))
                    boxed = builder_->CreateCall(boxI, {builder_->CreateZExt(v, Type::getInt64Ty(*ctx_))});
                else if (v->getType()->isDoubleTy())
                    boxed = builder_->CreateCall(boxF, {v});
                builder_->CreateCall(push, {lst, boxed});
            }
            return lst;
        }
        case AstKind::DictLiteral: {
            Type* i8p = PointerType::getUnqual(*ctx_);
            FunctionCallee create = module_->getOrInsertFunction(
                "s_dict_create", FunctionType::get(i8p, {}, false));
            FunctionCallee put = module_->getOrInsertFunction(
                "s_dict_put", FunctionType::get(Type::getVoidTy(*ctx_), {i8p, i8p, i8p}, false));
            FunctionCallee boxI = module_->getOrInsertFunction(
                "s_box_int", FunctionType::get(i8p, {Type::getInt64Ty(*ctx_)}, false));
            FunctionCallee boxF = module_->getOrInsertFunction(
                "s_box_float", FunctionType::get(i8p, {Type::getDoubleTy(*ctx_)}, false));
            FunctionCallee keyInt = module_->getOrInsertFunction(
                "s_key_from_int", FunctionType::get(i8p, {Type::getInt64Ty(*ctx_)}, false));
            Value* d = builder_->CreateCall(create, {}, "dict");
            for (auto& item : e->children) {
                Value* key = nullptr;
                if (!item->name.empty())
                    key = builder_->CreateGlobalString(item->name, "dkey");
                else if (item->left) {
                    key = emitExpr(item->left.get());
                    if (key->getType()->isIntegerTy())
                        key = builder_->CreateCall(keyInt, {builder_->CreateSExtOrTrunc(key, Type::getInt64Ty(*ctx_))});
                } else {
                    key = builder_->CreateGlobalString("", "dkey");
                }
                Value* val = item->right ? emitExpr(item->right.get()) : Constant::getNullValue(i8p);
                Value* boxed = val;
                if (val->getType()->isIntegerTy(64))
                    boxed = builder_->CreateCall(boxI, {val});
                else if (val->getType()->isIntegerTy(1))
                    boxed = builder_->CreateCall(boxI, {builder_->CreateZExt(val, Type::getInt64Ty(*ctx_))});
                else if (val->getType()->isDoubleTy())
                    boxed = builder_->CreateCall(boxF, {val});
                builder_->CreateCall(put, {d, key, boxed});
            }
            return d;
        }
        case AstKind::ArrayRef: {
            Type* i8p = PointerType::getUnqual(*ctx_);
            FunctionCallee listGet = module_->getOrInsertFunction(
                "s_list_get", FunctionType::get(i8p, {i8p, Type::getInt64Ty(*ctx_)}, false));
            FunctionCallee dictGet = module_->getOrInsertFunction(
                "s_dict_get", FunctionType::get(i8p, {i8p, i8p}, false));
            FunctionCallee unboxI = module_->getOrInsertFunction(
                "s_unbox_int", FunctionType::get(Type::getInt64Ty(*ctx_), {i8p}, false));
            FunctionCallee isbox = module_->getOrInsertFunction(
                "s_is_int_box", FunctionType::get(Type::getInt32Ty(*ctx_), {i8p}, false));
            FunctionCallee keyInt = module_->getOrInsertFunction(
                "s_key_from_int", FunctionType::get(i8p, {Type::getInt64Ty(*ctx_)}, false));
            Value* base = emitExpr(e->left.get());
            Value* idx = emitExpr(e->right.get());
            Value* slot = nullptr;
            if (idx->getType()->isIntegerTy()) {
                if (!idx->getType()->isIntegerTy(64))
                    idx = builder_->CreateSExtOrTrunc(idx, Type::getInt64Ty(*ctx_));
                slot = builder_->CreateCall(listGet, {base, idx});
            } else {
                slot = builder_->CreateCall(dictGet, {base, idx});
            }
            return slot;
        }
        case AstKind::TypeCast: {
            Value* v = emitExpr(e->left.get());
            if (!e->type) return v;
            std::string tn = e->type->text;
            if (tn.empty() && e->type->typeTok != TokenKind::Eof) {
                /* keyword types stored as typeTok */
            }
            TokenKind tk = e->type->typeTok;
            auto asInt = [&](Value* x) -> Value* {
                if (x->getType()->isIntegerTy(64)) return x;
                if (x->getType()->isIntegerTy())
                    return builder_->CreateSExtOrTrunc(x, Type::getInt64Ty(*ctx_));
                if (x->getType()->isFloatingPointTy())
                    return builder_->CreateFPToSI(x, Type::getInt64Ty(*ctx_));
                if (x->getType()->isPointerTy())
                    return builder_->CreatePtrToInt(x, Type::getInt64Ty(*ctx_));
                return ConstantInt::get(Type::getInt64Ty(*ctx_), 0);
            };
            auto asFloat = [&](Value* x) -> Value* {
                if (x->getType()->isDoubleTy()) return x;
                if (x->getType()->isIntegerTy())
                    return builder_->CreateSIToFP(x, Type::getDoubleTy(*ctx_));
                return ConstantFP::get(Type::getDoubleTy(*ctx_), 0.0);
            };
            auto asBool = [&](Value* x) -> Value* {
                if (x->getType()->isIntegerTy(1)) return x;
                if (x->getType()->isIntegerTy())
                    return builder_->CreateICmpNE(x, Constant::getNullValue(x->getType()));
                if (x->getType()->isFloatingPointTy())
                    return builder_->CreateFCmpONE(x, ConstantFP::get(x->getType(), 0.0));
                if (x->getType()->isPointerTy())
                    return builder_->CreateICmpNE(x, Constant::getNullValue(x->getType()));
                return ConstantInt::getFalse(*ctx_);
            };
            auto asStr = [&](Value* x) -> Value* {
                Type* i8p = PointerType::getUnqual(*ctx_);
                if (x->getType()->isPointerTy()) return x;
                if (x->getType()->isIntegerTy(64) || x->getType()->isIntegerTy()) {
                    FunctionCallee f = module_->getOrInsertFunction(
                        "s_str_from_int", FunctionType::get(i8p, {Type::getInt64Ty(*ctx_)}, false));
                    return builder_->CreateCall(f, {asInt(x)});
                }
                if (x->getType()->isFloatingPointTy()) {
                    FunctionCallee f = module_->getOrInsertFunction(
                        "s_str_from_float", FunctionType::get(i8p, {Type::getDoubleTy(*ctx_)}, false));
                    return builder_->CreateCall(f, {asFloat(x)});
                }
                return builder_->CreateGlobalString("", "empty");
            };
            if (tk == TokenKind::Integer || tk == TokenKind::Int || tk == TokenKind::Unsigned)
                return asInt(v);
            if (tk == TokenKind::Float) return asFloat(v);
            if (tk == TokenKind::Boolean || tk == TokenKind::Bool) return asBool(v);
            if (tk == TokenKind::String) return asStr(v);
            if (!tn.empty() || (e->type->left && e->type->left->kind == AstKind::CompoundName)) {
                if (e->type->left && e->type->left->kind == AstKind::CompoundName &&
                    !e->type->left->nameParts.empty())
                    tn = e->type->left->nameParts.back();
                Type* i8p = PointerType::getUnqual(*ctx_);
                if (!v->getType()->isPointerTy())
                    v = builder_->CreateIntToPtr(asInt(v), i8p);
                FunctionCallee isfn = module_->getOrInsertFunction(
                    "s_is_classname",
                    FunctionType::get(Type::getInt1Ty(*ctx_), {i8p, i8p}, false));
                Value* nm = builder_->CreateGlobalString(tn.empty() ? "object" : tn, "asname");
                Value* ok = builder_->CreateCall(isfn, {v, nm}, "asok");
                Value* nul = Constant::getNullValue(v->getType());
                return builder_->CreateSelect(ok, v, nul, "ascast");
            }
            return v;
        }
        case AstKind::TypeOf: {
            if (e->left) {
                Value* v = emitExpr(e->left.get());
                if (v && v->getType()->isPointerTy()) {
                    Type* i8p = PointerType::getUnqual(*ctx_);
                    Value* isn = builder_->CreateICmpEQ(
                        v, ConstantPointerNull::get(llvm::cast<PointerType>(v->getType())));
                    llvm::Function* pf = builder_->GetInsertBlock()->getParent();
                    auto* bbNull = BasicBlock::Create(*ctx_, "ty.null", pf);
                    auto* bbObj  = BasicBlock::Create(*ctx_, "ty.obj", pf);
                    auto* bbJoin = BasicBlock::Create(*ctx_, "ty.join", pf);
                    builder_->CreateCondBr(isn, bbNull, bbObj);
                    builder_->SetInsertPoint(bbNull);
                    Value* nullName = builder_->CreateGlobalString("null", "tnnull");
                    builder_->CreateBr(bbJoin);
                    builder_->SetInsertPoint(bbObj);
                    FunctionCallee gcn = module_->getOrInsertFunction(
                        "s_get_classname", FunctionType::get(i8p, {i8p}, false));
                    Value* rt = builder_->CreateCall(gcn, {v}, "rttypename");
                    FunctionCallee slen = module_->getOrInsertFunction(
                        "strlen", FunctionType::get(Type::getInt64Ty(*ctx_), {i8p}, false));
                    Value* nlen = builder_->CreateCall(slen, {rt});
                    Value* empty = builder_->CreateICmpEQ(nlen, ConstantInt::get(Type::getInt64Ty(*ctx_), 0));
                    Value* fb = builder_->CreateGlobalString(e->text.empty() ? "object" : e->text, "tnfb");
                    Value* pick = builder_->CreateSelect(empty, fb, rt, "typename");
                    builder_->CreateBr(bbJoin);
                    builder_->SetInsertPoint(bbJoin);
                    auto* phi = builder_->CreatePHI(i8p, 2, "typename");
                    phi->addIncoming(nullName, bbNull);
                    phi->addIncoming(pick, bbObj);
                    return phi;
                }
            }
            std::string name = e->text.empty() ? "unknown" : e->text;
            return builder_->CreateGlobalString(name, "typename");
        }
        case AstKind::FunctionCall: {
            if (e->text == "super" && !e->className.empty()) {
                Type* i8p = PointerType::getUnqual(*ctx_);
                Value* self = nullptr;
                auto sit = locals_.find("self");
                if (sit != locals_.end())
                    self = builder_->CreateLoad(sit->second->getAllocatedType(), sit->second, "self");
                else
                    self = Constant::getNullValue(i8p);
                std::vector<Value*> args;
                args.push_back(self);
                for (auto& a : e->args) args.push_back(emitExpr(a.get()));
                std::string callee;
                if (e->name == "create")
                    callee = e->className + "_init";
                else
                    callee = e->className + "_" + e->name;
                Function* fn = module_->getFunction(callee);
                if (!fn) {
                    // declare loosely
                    std::vector<Type*> pts;
                    pts.push_back(i8p);
                    for (auto& a : args)
                        if (&a != &args[0]) pts.push_back(a->getType());
                    FunctionType* fty = FunctionType::get(Type::getVoidTy(*ctx_), pts, false);
                    fn = Function::Create(fty, Function::ExternalLinkage, callee, module_);
                }
                return builder_->CreateCall(fn, args);
            }
            if (inMethod_ && !currentClass_.empty() && e->text != "super") {
                auto vit = virtIndex_.find(currentClass_);
                if (vit != virtIndex_.end() && vit->second.count(e->name) && locals_.count("self")) {
                    unsigned slot = vit->second[e->name];
                    auto clsIt = classes_.find(currentClass_);
                    Value* self = builder_->CreateLoad(
                        locals_["self"]->getAllocatedType(), locals_["self"], "self");
                    Value* vtp = builder_->CreateStructGEP(clsIt->second, self, 0, "vptr");
                    Value* vt = builder_->CreateLoad(PointerType::getUnqual(*ctx_), vtp, "vt");
                    Value* slotp = builder_->CreateGEP(
                        PointerType::getUnqual(*ctx_), vt,
                        ConstantInt::get(Type::getInt32Ty(*ctx_), slot), "vslot");
                    Value* fptr = builder_->CreateLoad(PointerType::getUnqual(*ctx_), slotp, "vfn");
                    std::vector<Value*> args;
                    args.push_back(self);
                    for (auto& a : e->args) args.push_back(emitExpr(a.get()));
                    std::vector<Type*> pts;
                    pts.push_back(PointerType::getUnqual(*ctx_));
                    for (size_t i = 1; i < args.size(); ++i) pts.push_back(args[i]->getType());
                    FunctionType* ft = FunctionType::get(Type::getVoidTy(*ctx_), pts, false);
                    return builder_->CreateCall(ft, fptr, args);
                }
            }
            Function* fn = module_->getFunction(e->name);
            if (!fn) return ConstantInt::get(Type::getInt64Ty(*ctx_), 0);
            std::vector<Value*> args;
            for (auto& a : e->args) args.push_back(emitExpr(a.get()));
            return builder_->CreateCall(fn, args);
        }
        case AstKind::FormatString: {
            // Runtime: s_str_fmt_start / s_str_fmt_put / s_str_fmt_finish
            Type* i8p = PointerType::getUnqual(*ctx_);
            FunctionCallee startFn = module_->getOrInsertFunction(
                "s_str_fmt_start",
                FunctionType::get(Type::getVoidTy(*ctx_), {i8p}, false));
            FunctionCallee putFn = module_->getOrInsertFunction(
                "s_str_fmt_put",
                FunctionType::get(Type::getVoidTy(*ctx_), {i8p, i8p}, false));
            FunctionCallee finFn = module_->getOrInsertFunction(
                "s_str_fmt_finish",
                FunctionType::get(i8p, {}, false));
            FunctionCallee fromInt = module_->getOrInsertFunction(
                "s_str_from_int",
                FunctionType::get(i8p, {Type::getInt64Ty(*ctx_)}, false));
            FunctionCallee fromFloat = module_->getOrInsertFunction(
                "s_str_from_float",
                FunctionType::get(i8p, {Type::getDoubleTy(*ctx_)}, false));
            FunctionCallee fromC = module_->getOrInsertFunction(
                "s_str_from_cstr",
                FunctionType::get(i8p, {i8p}, false));

            // Strip quotes from format pattern
            std::string raw = e->text;
            if (raw.size() >= 2 && (raw.front() == '"' || raw.front() == '\''))
                raw = raw.substr(1, raw.size() - 2);
            Value* fmtStr = builder_->CreateGlobalString(raw, "fmt");
            builder_->CreateCall(startFn, {fmtStr});

            for (auto& arg : e->children) {
                if (!arg) continue;
                Value* val = arg->left ? emitExpr(arg->left.get()) : builder_->CreateGlobalString("", "empty");
                Value* asStr = val;
                Type* vt = val->getType();
                if (vt->isIntegerTy(64)) {
                    asStr = builder_->CreateCall(fromInt, {val});
                } else if (vt->isIntegerTy(1)) {
                    asStr = builder_->CreateCall(fromInt,
                        {builder_->CreateZExt(val, Type::getInt64Ty(*ctx_))});
                } else if (vt->isFloatingPointTy()) {
                    asStr = builder_->CreateCall(fromFloat, {val});
                } else if (vt->isPointerTy()) {
                    FunctionCallee isbox = module_->getOrInsertFunction(
                        "s_is_int_box", FunctionType::get(Type::getInt32Ty(*ctx_), {i8p}, false));
                    FunctionCallee unboxI = module_->getOrInsertFunction(
                        "s_unbox_int", FunctionType::get(Type::getInt64Ty(*ctx_), {i8p}, false));
                    Value* ib = builder_->CreateCall(isbox, {val});
                    Value* isI = builder_->CreateICmpNE(ib, ConstantInt::get(Type::getInt32Ty(*ctx_), 0));
                    Value* iv = builder_->CreateCall(unboxI, {val});
                    Value* istr = builder_->CreateCall(fromInt, {iv});
                    asStr = builder_->CreateSelect(isI, istr, val);
                } else {
                    asStr = builder_->CreateCall(fromC, {builder_->CreateGlobalString("?", "q")});
                }
                Value* nameV = Constant::getNullValue(i8p);
                if (!arg->name.empty())
                    nameV = builder_->CreateGlobalString(arg->name, "fn");
                builder_->CreateCall(putFn, {nameV, asStr});
            }
            return builder_->CreateCall(finFn, {});
        }
        default:
            return ConstantInt::get(Type::getInt64Ty(*ctx_), 0);
    }
}

void Codegen::emitStmt(AstNode* s) {
    if (!s) return;
    setDebugLoc(s->line, s->column);
    switch (s->kind) {
        case AstKind::Assignment: {
            Value* rhs = emitExpr(s->right.get());
            AstNode* lhs = s->left.get();
            bool rhsNull = s->right && s->right->kind == AstKind::Literal &&
                s->right->litKind == TokenKind::Null;
            if (rhsNull) {
                std::string lname;
                if (lhs && lhs->kind == AstKind::Identifier) lname = lhs->name;
                else if (lhs && lhs->kind == AstKind::CompoundRef && lhs->children.size() == 1 &&
                         lhs->children[0]->kind == AstKind::Identifier)
                    lname = lhs->children[0]->name;
                if (!lname.empty() && locals_.count(lname)) {
                    Type* aty = locals_[lname]->getAllocatedType();
                    Value* z = Constant::getNullValue(aty);
                    builder_->CreateStore(z, locals_[lname]);
                    break;
                }
            }
            bool factory = s->right && s->right->kind == AstKind::CompoundRef &&
                s->right->children.size() >= 2 &&
                s->right->children[1]->kind == AstKind::FunctionCall &&
                s->right->children[1]->name == "create";
            {
                std::string lname;
                if (lhs && lhs->kind == AstKind::Identifier) lname = lhs->name;
                else if (lhs && lhs->kind == AstKind::CompoundRef && lhs->children.size() == 1 &&
                         lhs->children[0]->kind == AstKind::Identifier)
                    lname = lhs->children[0]->name;
                if (!s->className.empty() && !s->text.empty() && s->text != s->className &&
                    !lname.empty() && locals_.count(lname)) {
                    Type* aty = locals_[lname]->getAllocatedType();
                    Value* casted = rhs;
                    if (casted->getType() != aty && aty->isPointerTy())
                        casted = builder_->CreateBitCast(casted, aty, "upcast");
                    builder_->CreateStore(casted, locals_[lname]);
                    break;
                }
            }
            if (!s->className.empty() && !factory &&
                !(s->right && s->right->kind == AstKind::UnaryOp && s->right->op == TokenKind::Copy)) {
                Function* asg = module_->getFunction(s->className + "_assign");
                Function* cpy = module_->getFunction(s->className + "_copy");
                std::string lname;
                if (lhs->kind == AstKind::Identifier) lname = lhs->name;
                else if (lhs->kind == AstKind::CompoundRef && lhs->children.size() == 1 &&
                         lhs->children[0]->kind == AstKind::Identifier)
                    lname = lhs->children[0]->name;
                if (!lname.empty() && locals_.count(lname) && asg && cpy) {
                    Value* dst = builder_->CreateLoad(locals_[lname]->getAllocatedType(), locals_[lname], "dst");
                    Value* isnull = builder_->CreateICmpEQ(dst, Constant::getNullValue(dst->getType()), "dnull");
                    BasicBlock* doCopy = BasicBlock::Create(*ctx_, "copy.ctor", currentFn_);
                    BasicBlock* doAsg = BasicBlock::Create(*ctx_, "copy.asg", currentFn_);
                    BasicBlock* join = BasicBlock::Create(*ctx_, "copy.join", currentFn_);
                    builder_->CreateCondBr(isnull, doCopy, doAsg);
                    builder_->SetInsertPoint(doCopy);
                    Value* nw = builder_->CreateCall(cpy, {rhs}, "cloned");
                    builder_->CreateStore(nw, locals_[lname]);
                    builder_->CreateBr(join);
                    builder_->SetInsertPoint(doAsg);
                    builder_->CreateCall(asg, {dst, rhs});
                    builder_->CreateBr(join);
                    builder_->SetInsertPoint(join);
                    break;
                }
            }
            if (lhs && lhs->kind == AstKind::ArrayRef) {
                Type* i8p = PointerType::getUnqual(*ctx_);
                FunctionCallee boxI = module_->getOrInsertFunction(
                    "s_box_int", FunctionType::get(i8p, {Type::getInt64Ty(*ctx_)}, false));
                FunctionCallee boxF = module_->getOrInsertFunction(
                    "s_box_float", FunctionType::get(i8p, {Type::getDoubleTy(*ctx_)}, false));
                FunctionCallee listSet = module_->getOrInsertFunction(
                    "s_list_set",
                    FunctionType::get(Type::getVoidTy(*ctx_), {i8p, Type::getInt64Ty(*ctx_), i8p}, false));
                FunctionCallee dictPut = module_->getOrInsertFunction(
                    "s_dict_put",
                    FunctionType::get(Type::getVoidTy(*ctx_), {i8p, i8p, i8p}, false));
                FunctionCallee keyInt = module_->getOrInsertFunction(
                    "s_key_from_int",
                    FunctionType::get(i8p, {Type::getInt64Ty(*ctx_)}, false));
                Value* base = emitExpr(lhs->left.get());
                Value* idx = emitExpr(lhs->right.get());
                Value* boxed = rhs;
                if (rhs->getType()->isIntegerTy(64))
                    boxed = builder_->CreateCall(boxI, {rhs});
                else if (rhs->getType()->isIntegerTy(1))
                    boxed = builder_->CreateCall(boxI, {builder_->CreateZExt(rhs, Type::getInt64Ty(*ctx_))});
                else if (rhs->getType()->isDoubleTy())
                    boxed = builder_->CreateCall(boxF, {rhs});
                if (idx->getType()->isIntegerTy(64) || idx->getType()->isIntegerTy()) {
                    if (!idx->getType()->isIntegerTy(64))
                        idx = builder_->CreateSExtOrTrunc(idx, Type::getInt64Ty(*ctx_));
                    builder_->CreateCall(listSet, {base, idx, boxed});
                } else {
                    Value* key = idx;
                    if (idx->getType()->isIntegerTy())
                        key = builder_->CreateCall(keyInt, {builder_->CreateSExtOrTrunc(idx, Type::getInt64Ty(*ctx_))});
                    builder_->CreateCall(dictPut, {base, key, boxed});
                }
                break;
            }
            std::string name;
            if (lhs->kind == AstKind::Identifier) name = lhs->name;
            else if (lhs->kind == AstKind::CompoundRef && lhs->children.size() == 1 &&
                     lhs->children[0]->kind == AstKind::Identifier)
                name = lhs->children[0]->name;

            if (lhs && lhs->kind == AstKind::CompoundRef && lhs->children.size() >= 2 &&
                lhs->children.back()->kind == AstKind::Identifier) {
                std::string fld = lhs->children.back()->name;
                Value* obj = nullptr;
                if (lhs->children[0]->kind == AstKind::Identifier)
                    obj = emitExpr(lhs->children[0].get());
                unsigned idx = 0;
                StructType* sty = nullptr;
                std::string owner;
                for (auto& kv : fieldIndex_) {
                    auto fit = kv.second.find(fld);
                    if (fit != kv.second.end()) {
                        idx = fit->second;
                        owner = kv.first;
                        auto cit = classes_.find(kv.first);
                        if (cit != classes_.end()) sty = cit->second;
                        break;
                    }
                }
                if (obj && sty && idx > 0) {
                    emitAccessCheck(owner, fld);
                    Value* gep = builder_->CreateStructGEP(sty, obj, idx, fld);
                    builder_->CreateStore(rhs, gep);
                }
                break;
            }
            if (!name.empty()) {
                if (inGenerator_) {
                    auto gs = genSlots_.find(name);
                    if (gs == genSlots_.end())
                        gs = genSlots_.insert({name, genAllocSlot(name)}).first;
                    Value* v = rhs;
                    if (!v->getType()->isIntegerTy(64) && v->getType()->isIntegerTy())
                        v = builder_->CreateSExtOrTrunc(v, Type::getInt64Ty(*ctx_));
                    genSetInt(gs->second, v);
                    break;
                }
                auto it = locals_.find(name);
                if (it != locals_.end()) {
                    builder_->CreateStore(rhs, it->second);
                    break;
                }
                if (inMethod_ && !currentClass_.empty()) {
                    auto selfIt = locals_.find("self");
                    auto clsIt = classes_.find(currentClass_);
                    if (selfIt != locals_.end() && clsIt != classes_.end()) {
                        Value* self = builder_->CreateLoad(
                            PointerType::getUnqual(clsIt->second), selfIt->second, "self.ptr");
                        unsigned idx = 1;
                        auto fi = fieldIndex_.find(currentClass_);
                        if (fi != fieldIndex_.end()) {
                            auto it2 = fi->second.find(name);
                            if (it2 != fi->second.end()) idx = it2->second;
                        }
                        emitAccessCheck(currentClass_, name);
                        Value* gep = builder_->CreateStructGEP(clsIt->second, self, idx, name + ".ptr");
                        builder_->CreateStore(rhs, gep);
                    }
                }
            }
            break;
        }
        case AstKind::Return: {
            if (s->left) {
                builder_->CreateRet(emitExpr(s->left.get()));
            } else {
                if (currentFn_->getReturnType()->isVoidTy())
                    builder_->CreateRetVoid();
                else
                    builder_->CreateRet(Constant::getNullValue(currentFn_->getReturnType()));
            }
            // Unreachable block for further code
            BasicBlock* cont = BasicBlock::Create(*ctx_, "after.ret", currentFn_);
            builder_->SetInsertPoint(cont);
            break;
        }
        case AstKind::If: {
            Value* cond = ConstantInt::getTrue(*ctx_);
            if (s->condition) {
                cond = emitExpr(s->condition.get());
                if (!cond->getType()->isIntegerTy(1))
                    cond = builder_->CreateICmpNE(cond, Constant::getNullValue(cond->getType()), "tobool");
            }

            BasicBlock* thenBB = BasicBlock::Create(*ctx_, "if.then", currentFn_);
            BasicBlock* elseBB = BasicBlock::Create(*ctx_, "if.else", currentFn_);
            BasicBlock* mergeBB = BasicBlock::Create(*ctx_, "if.end", currentFn_);
            builder_->CreateCondBr(cond, thenBB, elseBB);

            builder_->SetInsertPoint(thenBB);
            emitBlock(s->body.get());
            emitBrIfOpen(mergeBB);

            builder_->SetInsertPoint(elseBB);
            if (s->elseBody) {
                if (s->elseBody->kind == AstKind::If) emitStmt(s->elseBody.get());
                else emitBlock(s->elseBody.get());
            }
            emitBrIfOpen(mergeBB);

            builder_->SetInsertPoint(mergeBB);
            break;
        }
        case AstKind::While: {
            BasicBlock* condBB = BasicBlock::Create(*ctx_, "while.cond", currentFn_);
            BasicBlock* bodyBB = BasicBlock::Create(*ctx_, "while.body", currentFn_);
            BasicBlock* endBB  = BasicBlock::Create(*ctx_, "while.end", currentFn_);
            builder_->CreateBr(condBB);
            builder_->SetInsertPoint(condBB);
            Value* cond = s->condition ? emitExpr(s->condition.get())
                                       : ConstantInt::getTrue(*ctx_);
            if (!cond->getType()->isIntegerTy(1))
                cond = builder_->CreateICmpNE(cond, Constant::getNullValue(cond->getType()), "tobool");
            builder_->CreateCondBr(cond, bodyBB, endBB);
            loops_.push_back({condBB, endBB});
            builder_->SetInsertPoint(bodyBB);
            emitBlock(s->body.get());
            emitBrIfOpen(condBB);
            loops_.pop_back();
            builder_->SetInsertPoint(endBB);
            break;
        }
        case AstKind::DoWhile: {
            BasicBlock* bodyBB = BasicBlock::Create(*ctx_, "do.body", currentFn_);
            BasicBlock* condBB = BasicBlock::Create(*ctx_, "do.cond", currentFn_);
            BasicBlock* endBB  = BasicBlock::Create(*ctx_, "do.end", currentFn_);
            builder_->CreateBr(bodyBB);
            loops_.push_back({condBB, endBB});
            builder_->SetInsertPoint(bodyBB);
            emitBlock(s->body.get());
            emitBrIfOpen(condBB);
            builder_->SetInsertPoint(condBB);
            Value* cond = s->condition ? emitExpr(s->condition.get())
                                       : ConstantInt::getTrue(*ctx_);
            if (!cond->getType()->isIntegerTy(1))
                cond = builder_->CreateICmpNE(cond, Constant::getNullValue(cond->getType()), "tobool");
            builder_->CreateCondBr(cond, bodyBB, endBB);
            loops_.pop_back();
            builder_->SetInsertPoint(endBB);
            break;
        }
        case AstKind::For: {
            // Iterator for: for (gen() as x) when range is a pointer (SGen*)
            if (s->condition && !s->name.empty() &&
                (s->condition->kind == AstKind::FunctionCall ||
                 s->condition->kind == AstKind::CompoundRef)) {
                Type* i8p = PointerType::getUnqual(*ctx_);
                Value* gen = emitExpr(s->condition.get());
                if (gen && gen->getType()->isPointerTy()) {
                    FunctionCallee gnext = module_->getOrInsertFunction(
                        "s_gen_next",
                        FunctionType::get(Type::getInt32Ty(*ctx_), {i8p}, false));
                    bool strIter = s->type && (s->type->typeTok == TokenKind::String);
                    FunctionCallee gint = module_->getOrInsertFunction(
                        "s_gen_int",
                        FunctionType::get(Type::getInt64Ty(*ctx_), {i8p}, false));
                    FunctionCallee gval = module_->getOrInsertFunction(
                        "s_gen_value",
                        FunctionType::get(i8p, {i8p}, false));
                    AllocaInst* iv = createEntryAlloca(
                        currentFn_,
                        strIter ? (Type*)i8p : (Type*)Type::getInt64Ty(*ctx_),
                        s->name);
                    locals_[s->name] = iv;
                    BasicBlock* condBB = BasicBlock::Create(*ctx_, "for.iter.cond", currentFn_);
                    BasicBlock* bodyBB = BasicBlock::Create(*ctx_, "for.iter.body", currentFn_);
                    BasicBlock* endBB  = BasicBlock::Create(*ctx_, "for.iter.end", currentFn_);
                    builder_->CreateBr(condBB);
                    builder_->SetInsertPoint(condBB);
                    Value* has = builder_->CreateCall(gnext, {gen}, "has");
                    Value* ok = builder_->CreateICmpNE(has, ConstantInt::get(Type::getInt32Ty(*ctx_), 0), "ok");
                    builder_->CreateCondBr(ok, bodyBB, endBB);
                    loops_.push_back({condBB, endBB});
                    builder_->SetInsertPoint(bodyBB);
                    Value* val = strIter
                        ? builder_->CreateCall(gval, {gen}, s->name)
                        : builder_->CreateCall(gint, {gen}, s->name);
                    builder_->CreateStore(val, iv);
                    emitBlock(s->body.get());
                    emitBrIfOpen(condBB);
                    loops_.pop_back();
                    builder_->SetInsertPoint(endBB);
                    break;
                }
            }
            BasicBlock* condBB = BasicBlock::Create(*ctx_, "for.cond", currentFn_);
            BasicBlock* bodyBB = BasicBlock::Create(*ctx_, "for.body", currentFn_);
            BasicBlock* incBB  = BasicBlock::Create(*ctx_, "for.inc", currentFn_);
            BasicBlock* endBB  = BasicBlock::Create(*ctx_, "for.end", currentFn_);

            AllocaInst* iv = nullptr;
            if (!s->name.empty()) {
                iv = createEntryAlloca(currentFn_, Type::getInt64Ty(*ctx_), s->name);
                locals_[s->name] = iv;
                builder_->CreateStore(ConstantInt::get(Type::getInt64Ty(*ctx_), 0), iv);
            }
            builder_->CreateBr(condBB);
            builder_->SetInsertPoint(condBB);
            Value* cond = nullptr;
            if (iv && s->condition) {
                Value* lim = emitExpr(s->condition.get());
                if (!lim->getType()->isIntegerTy(64)) {
                    if (lim->getType()->isIntegerTy())
                        lim = builder_->CreateSExtOrTrunc(lim, Type::getInt64Ty(*ctx_));
                    else
                        lim = ConstantInt::get(Type::getInt64Ty(*ctx_), 0);
                }
                Value* cur = builder_->CreateLoad(Type::getInt64Ty(*ctx_), iv, s->name);
                cond = builder_->CreateICmpSLT(cur, lim, "for.cmp");
            } else if (s->condition) {
                cond = emitExpr(s->condition.get());
                if (!cond->getType()->isIntegerTy(1))
                    cond = builder_->CreateICmpNE(cond, Constant::getNullValue(cond->getType()), "tobool");
            } else {
                cond = ConstantInt::getTrue(*ctx_);
            }
            builder_->CreateCondBr(cond, bodyBB, endBB);
            loops_.push_back({incBB, endBB});
            builder_->SetInsertPoint(bodyBB);
            emitBlock(s->body.get());
            emitBrIfOpen(incBB);
            builder_->SetInsertPoint(incBB);
            if (iv) {
                Value* cur = builder_->CreateLoad(Type::getInt64Ty(*ctx_), iv, s->name);
                Value* nxt = builder_->CreateAdd(cur, ConstantInt::get(Type::getInt64Ty(*ctx_), 1), "inc");
                builder_->CreateStore(nxt, iv);
            }
            builder_->CreateBr(condBB);
            loops_.pop_back();
            builder_->SetInsertPoint(endBB);
            break;
        }
        case AstKind::Inline: {
            std::string fn = "simple_inline_" + std::to_string(inlineSnippets_.size());
            InlineSnippet sn;
            sn.name = fn;
            sn.body = s->text;
            std::vector<Type*> pts;
            std::vector<Value*> args;
            Type* i8p = PointerType::getUnqual(*ctx_);
            for (auto& nm : s->nameParts) {
                auto it = locals_.find(nm);
                if (it == locals_.end()) {
                    std::fprintf(stderr, "inline: unknown local '%s'\n", nm.c_str());
                    continue;
                }
                AllocaInst* a = it->second;
                Type* ty = a->getAllocatedType();
                char k = 'i';
                if (ty->isDoubleTy()) k = 'f';
                else if (ty->isPointerTy()) k = 'p';
                sn.params.push_back(nm);
                sn.kinds.push_back(k);
                pts.push_back(i8p);
                args.push_back(a);
            }
            inlineSnippets_.push_back(std::move(sn));
            FunctionCallee cf = module_->getOrInsertFunction(
                fn, FunctionType::get(Type::getVoidTy(*ctx_), pts, false));
            builder_->CreateCall(cf, args);
            break;
        }
        case AstKind::Yield: {
            if (!inGenerator_ || !genPtr_) {
                std::fprintf(stderr, "yield outside generator\n");
                break;
            }
            Type* i8p = PointerType::getUnqual(*ctx_);
            Value* v = s->left ? emitExpr(s->left.get())
                               : ConstantInt::get(Type::getInt64Ty(*ctx_), 0);
            Value* boxed = nullptr;
            if (genYieldTok_ == TokenKind::Float) {
                FunctionCallee boxF = module_->getOrInsertFunction(
                    "s_box_float", FunctionType::get(i8p, {Type::getDoubleTy(*ctx_)}, false));
                if (!v->getType()->isDoubleTy()) {
                    if (v->getType()->isIntegerTy())
                        v = builder_->CreateSIToFP(v, Type::getDoubleTy(*ctx_));
                    else
                        v = ConstantFP::get(Type::getDoubleTy(*ctx_), 0.0);
                }
                boxed = builder_->CreateCall(boxF, {v});
            } else if (genYieldTok_ == TokenKind::String) {
                boxed = v->getType()->isPointerTy() ? v
                    : builder_->CreateGlobalString("", "ys");
            } else {
                FunctionCallee boxI = module_->getOrInsertFunction(
                    "s_box_int", FunctionType::get(i8p, {Type::getInt64Ty(*ctx_)}, false));
                if (!v->getType()->isIntegerTy(64)) {
                    if (v->getType()->isIntegerTy())
                        v = builder_->CreateSExtOrTrunc(v, Type::getInt64Ty(*ctx_));
                    else
                        v = ConstantInt::get(Type::getInt64Ty(*ctx_), 0);
                }
                boxed = builder_->CreateCall(boxI, {v});
            }
            FunctionCallee yfn = module_->getOrInsertFunction(
                "s_gen_yield",
                FunctionType::get(Type::getVoidTy(*ctx_), {i8p, i8p}, false));
            builder_->CreateCall(yfn, {genPtr_, boxed});
            int nid = ++genYieldId_;
            FunctionCallee setSt = module_->getOrInsertFunction(
                "s_gen_set_state",
                FunctionType::get(Type::getVoidTy(*ctx_), {i8p, Type::getInt32Ty(*ctx_)}, false));
            builder_->CreateCall(setSt, {genPtr_, ConstantInt::get(Type::getInt32Ty(*ctx_), nid)});
            builder_->CreateRet(ConstantInt::get(Type::getInt32Ty(*ctx_), 1));
            BasicBlock* resume = BasicBlock::Create(*ctx_, "yield.resume", currentFn_);
            if (genSwitch_)
                genSwitch_->addCase(cast<ConstantInt>(ConstantInt::get(Type::getInt32Ty(*ctx_), nid)), resume);
            builder_->SetInsertPoint(resume);
            break;
        }
        case AstKind::Break: {
            if (loops_.empty()) {
                std::fprintf(stderr, "break outside loop\n");
                break;
            }
            builder_->CreateBr(loops_.back().end);
            BasicBlock* dead = BasicBlock::Create(*ctx_, "after.break", currentFn_);
            builder_->SetInsertPoint(dead);
            break;
        }
        case AstKind::Continue: {
            if (loops_.empty()) {
                std::fprintf(stderr, "continue outside loop\n");
                break;
            }
            builder_->CreateBr(loops_.back().cont);
            BasicBlock* dead = BasicBlock::Create(*ctx_, "after.cont", currentFn_);
            builder_->SetInsertPoint(dead);
            break;
        }
        case AstKind::Exit: {
            FunctionCallee exitFn = module_->getOrInsertFunction(
                "exit",
                FunctionType::get(Type::getVoidTy(*ctx_), {Type::getInt32Ty(*ctx_)}, false));
            Value* code = ConstantInt::get(Type::getInt32Ty(*ctx_), 0);
            if (s->left) {
                Value* v = emitExpr(s->left.get());
                if (v->getType()->isIntegerTy(32))
                    code = v;
                else if (v->getType()->isIntegerTy())
                    code = builder_->CreateTrunc(v, Type::getInt32Ty(*ctx_));
            }
            builder_->CreateCall(exitFn, {code});
            builder_->CreateUnreachable();
            BasicBlock* dead = BasicBlock::Create(*ctx_, "after.exit", currentFn_);
            builder_->SetInsertPoint(dead);
            break;
        }
        case AstKind::DataDecl: {
            if (inGenerator_) {
                int sl = genAllocSlot(s->name);
                if (s->init) {
                    Value* v = emitExpr(s->init.get());
                    if (!v->getType()->isIntegerTy(64) && v->getType()->isIntegerTy())
                        v = builder_->CreateSExtOrTrunc(v, Type::getInt64Ty(*ctx_));
                    if (v->getType()->isIntegerTy(64))
                        genSetInt(sl, v);
                }
                break;
            }
            Type* ty = mapType(s->type.get());
            AllocaInst* alloca = createEntryAlloca(currentFn_, ty, s->name);
            locals_[s->name] = alloca;
            if (!s->className.empty()) localClass_[s->name] = s->className;
            if (s->init) {
                Value* iv = emitExpr(s->init.get());
                bool factory = s->init->kind == AstKind::CompoundRef &&
                    s->init->children.size() >= 2 &&
                    s->init->children[1]->kind == AstKind::FunctionCall &&
                    s->init->children[1]->name == "create";
                if (!s->className.empty() && !factory) {
                    Function* cf = module_->getFunction(s->className + "_copy");
                    if (cf) iv = builder_->CreateCall(cf, {iv}, "copyinit");
                }
                builder_->CreateStore(iv, alloca);
            }
            break;
        }
        case AstKind::Print: {
            // print expr → convert to string and s_print_str; then newline
            Type* i8p = PointerType::getUnqual(*ctx_);
            FunctionCallee printStr = module_->getOrInsertFunction(
                "s_print_str",
                FunctionType::get(Type::getVoidTy(*ctx_), {i8p}, false));
            FunctionCallee printInt = module_->getOrInsertFunction(
                "s_print_int",
                FunctionType::get(Type::getVoidTy(*ctx_), {Type::getInt64Ty(*ctx_)}, false));
            FunctionCallee printFloat = module_->getOrInsertFunction(
                "s_print_float",
                FunctionType::get(Type::getVoidTy(*ctx_), {Type::getDoubleTy(*ctx_)}, false));
            FunctionCallee printBool = module_->getOrInsertFunction(
                "s_print_bool",
                FunctionType::get(Type::getVoidTy(*ctx_), {Type::getInt1Ty(*ctx_)}, false));
            FunctionCallee printLn = module_->getOrInsertFunction(
                "s_print_ln",
                FunctionType::get(Type::getVoidTy(*ctx_), {}, false));
            FunctionCallee fromInt = module_->getOrInsertFunction(
                "s_str_from_int",
                FunctionType::get(i8p, {Type::getInt64Ty(*ctx_)}, false));
            FunctionCallee fromFloat = module_->getOrInsertFunction(
                "s_str_from_float",
                FunctionType::get(i8p, {Type::getDoubleTy(*ctx_)}, false));

            if (s->left) {
                Value* v = emitExpr(s->left.get());
                Type* vt = v->getType();
                if (vt->isPointerTy()) {
                    Value* isn = builder_->CreateICmpEQ(
                        v, ConstantPointerNull::get(llvm::cast<PointerType>(vt)));
                    llvm::Function* pf = builder_->GetInsertBlock()->getParent();
                    auto* bbNull = BasicBlock::Create(*ctx_, "prn.null", pf);
                    auto* bbStr  = BasicBlock::Create(*ctx_, "prn.str", pf);
                    auto* bbJoin = BasicBlock::Create(*ctx_, "prn.join", pf);
                    builder_->CreateCondBr(isn, bbNull, bbStr);
                    builder_->SetInsertPoint(bbNull);
                    builder_->CreateCall(printStr, {builder_->CreateGlobalString("null", "nultxt")});
                    builder_->CreateBr(bbJoin);
                    builder_->SetInsertPoint(bbStr);
                    FunctionCallee isbox = module_->getOrInsertFunction(
                        "s_is_int_box", FunctionType::get(Type::getInt32Ty(*ctx_), {i8p}, false));
                    Value* ib = builder_->CreateCall(isbox, {v});
                    Value* isI = builder_->CreateICmpNE(ib, ConstantInt::get(Type::getInt32Ty(*ctx_), 0));
                    auto* bbInt = BasicBlock::Create(*ctx_, "prn.ibox", pf);
                    auto* bbRaw = BasicBlock::Create(*ctx_, "prn.raw", pf);
                    builder_->CreateCondBr(isI, bbInt, bbRaw);
                    builder_->SetInsertPoint(bbInt);
                    FunctionCallee unboxI = module_->getOrInsertFunction(
                        "s_unbox_int", FunctionType::get(Type::getInt64Ty(*ctx_), {i8p}, false));
                    Value* iv = builder_->CreateCall(unboxI, {v});
                    builder_->CreateCall(printInt, {iv});
                    builder_->CreateBr(bbJoin);
                    builder_->SetInsertPoint(bbRaw);
                    builder_->CreateCall(printStr, {v});
                    builder_->CreateBr(bbJoin);
                    builder_->SetInsertPoint(bbJoin);
                } else if (vt->isIntegerTy(1)) {
                    builder_->CreateCall(printBool, {v});
                } else if (vt->isIntegerTy()) {
                    if (!vt->isIntegerTy(64))
                        v = builder_->CreateSExtOrTrunc(v, Type::getInt64Ty(*ctx_));
                    builder_->CreateCall(printInt, {v});
                } else if (vt->isFloatingPointTy()) {
                    if (!vt->isDoubleTy())
                        v = builder_->CreateFPExt(v, Type::getDoubleTy(*ctx_));
                    builder_->CreateCall(printFloat, {v});
                } else {
                    builder_->CreateCall(printStr, {builder_->CreateGlobalString("?", "pq")});
                }
            }
            builder_->CreateCall(printLn, {});
            break;
        }
        case AstKind::Raise: {
            // raise(ExceptionName) -> s_raise("ExceptionName")
            std::string ename = "Error";
            if (s->left && s->left->kind == AstKind::CompoundName && !s->left->nameParts.empty())
                ename = s->left->nameParts.back();
            FunctionCallee raiseFn = module_->getOrInsertFunction(
                "s_raise",
                FunctionType::get(Type::getVoidTy(*ctx_), {PointerType::getUnqual(*ctx_)}, false));
            Value* str = builder_->CreateGlobalString(ename, "exname");
            builder_->CreateCall(raiseFn, {str});
            builder_->CreateUnreachable();
            BasicBlock* cont = BasicBlock::Create(*ctx_, "after.raise", currentFn_);
            builder_->SetInsertPoint(cont);
            break;
        }
        case AstKind::Try: {
            // setjmp-based try/except/finally using runtime + libc setjmp
            FunctionCallee enterFn = module_->getOrInsertFunction(
                "s_except_enter",
                FunctionType::get(PointerType::getUnqual(*ctx_), {}, false));
            FunctionCallee leaveFn = module_->getOrInsertFunction(
                "s_except_leave",
                FunctionType::get(Type::getVoidTy(*ctx_), {}, false));
            FunctionCallee matchFn = module_->getOrInsertFunction(
                "s_except_matches",
                FunctionType::get(Type::getInt1Ty(*ctx_), {PointerType::getUnqual(*ctx_)}, false));
            FunctionCallee clearFn = module_->getOrInsertFunction(
                "s_except_clear",
                FunctionType::get(Type::getVoidTy(*ctx_), {}, false));
            // setjmp(jmp_buf*)
            FunctionCallee setjmpFn = module_->getOrInsertFunction(
                "setjmp",
                FunctionType::get(Type::getInt32Ty(*ctx_), {PointerType::getUnqual(*ctx_)}, false));

            Value* jb = builder_->CreateCall(enterFn, {}, "jb");
            Value* sj = builder_->CreateCall(setjmpFn, {jb}, "sj");
            Value* isTry = builder_->CreateICmpEQ(sj, ConstantInt::get(Type::getInt32Ty(*ctx_), 0), "istry");

            BasicBlock* tryBB = BasicBlock::Create(*ctx_, "try.body", currentFn_);
            BasicBlock* catchBB = BasicBlock::Create(*ctx_, "try.catch", currentFn_);
            BasicBlock* finallyBB = BasicBlock::Create(*ctx_, "try.finally", currentFn_);
            BasicBlock* endBB = BasicBlock::Create(*ctx_, "try.end", currentFn_);
            builder_->CreateCondBr(isTry, tryBB, catchBB);

            // try body
            builder_->SetInsertPoint(tryBB);
            emitBlock(s->body.get());
            builder_->CreateCall(leaveFn, {});
            builder_->CreateBr(finallyBB);

            // catch: match except clauses
            builder_->SetInsertPoint(catchBB);
            BasicBlock* nextHandler = catchBB;
            for (size_t i = 0; i < s->children.size(); ++i) {
                auto& ex = s->children[i];
                if (ex->kind != AstKind::Except) continue;
                std::string ename = "Error";
                if (ex->left && ex->left->kind == AstKind::CompoundName && !ex->left->nameParts.empty())
                    ename = ex->left->nameParts.back();
                BasicBlock* handlerBB = BasicBlock::Create(*ctx_, "except.body", currentFn_);
                BasicBlock* skipBB = BasicBlock::Create(*ctx_, "except.skip", currentFn_);
                Value* enameV = builder_->CreateGlobalString(ename, "exmatch");
                Value* matched = builder_->CreateCall(matchFn, {enameV}, "matched");
                builder_->CreateCondBr(matched, handlerBB, skipBB);
                builder_->SetInsertPoint(handlerBB);
                if (!ex->name.empty()) {
                    Type* i8p = PointerType::getUnqual(*ctx_);
                    FunctionCallee enfn = module_->getOrInsertFunction(
                        "s_except_name", FunctionType::get(i8p, {}, false));
                    Value* nm = builder_->CreateCall(enfn, {}, "exname");
                    AllocaInst* a = createEntryAlloca(currentFn_, i8p, ex->name);
                    locals_[ex->name] = a;
                    builder_->CreateStore(nm, a);
                }
                emitBlock(ex->body.get());
                builder_->CreateCall(clearFn, {});
                builder_->CreateCall(leaveFn, {});
                builder_->CreateBr(finallyBB);
                builder_->SetInsertPoint(skipBB);
                nextHandler = skipBB;
            }
            // no handler matched – re-raise after leave
            builder_->CreateCall(leaveFn, {});
            FunctionCallee raiseFn = module_->getOrInsertFunction(
                "s_raise",
                FunctionType::get(Type::getVoidTy(*ctx_), {PointerType::getUnqual(*ctx_)}, false));
            Value* reraisestr = builder_->CreateGlobalString("Error", "reraise");
            builder_->CreateCall(raiseFn, {reraisestr});
            builder_->CreateUnreachable();

            // finally
            builder_->SetInsertPoint(finallyBB);
            if (s->elseBody) emitBlock(s->elseBody.get());
            builder_->CreateBr(endBB);
            builder_->SetInsertPoint(endBB);
            break;
        }
        case AstKind::CompoundRef:
        case AstKind::FunctionCall:
            emitExpr(s);
            break;
        case AstKind::Body:
            emitBlock(s);
            break;
        default:
            break;
    }
}

void Codegen::emitBrIfOpen(BasicBlock* dest) {
    if (!dest) return;
    BasicBlock* bb = builder_->GetInsertBlock();
    if (bb && !bb->getTerminator())
        builder_->CreateBr(dest);
}

void Codegen::emitBlock(AstNode* body) {
    if (!body) return;
    for (auto& c : body->children)
        emitStmt(c.get());
}

void Codegen::emitFunction(AstNode* fn, const std::string& className) {
    if (fn && fn->isGenerator) {
        emitGenerator(fn, className);
        return;
    }
    Type* retTy = fn->type ? mapType(fn->type.get()) : Type::getVoidTy(*ctx_);
    std::vector<Type*> params;
    if (!className.empty()) {
        auto it = classes_.find(className);
        params.push_back(it != classes_.end() ? PointerType::getUnqual(it->second)
                                              : PointerType::getUnqual(*ctx_));
    }
    for (auto& p : fn->params)
        params.push_back(mapType(p->type.get()));

    FunctionType* fty = FunctionType::get(retTy, params, false);
    std::string name = !fn->mangledName.empty() ? fn->mangledName
                       : (className.empty() ? fn->name : className + "_" + fn->name);
    Function* f = Function::Create(fty, Function::ExternalLinkage, name, module_);

    if (!fn->body) {
        if (fn->isVirtual) {
            /* abstract: emit stub that prints and returns */
            currentFn_ = f;
            BasicBlock* entry = BasicBlock::Create(*ctx_, "entry", f);
            builder_->SetInsertPoint(entry);
            FunctionCallee puts = module_->getOrInsertFunction(
                "puts",
                FunctionType::get(Type::getInt32Ty(*ctx_), {PointerType::getUnqual(*ctx_)}, false));
            builder_->CreateCall(puts, {builder_->CreateGlobalString(
                "abstract method " + name + " called", "abs")});
            if (retTy->isVoidTy()) builder_->CreateRetVoid();
            else builder_->CreateRet(Constant::getNullValue(retTy));
            currentFn_ = nullptr;
        }
        return;
    }

    currentFn_ = f;
    locals_.clear();
    currentSP_ = createDIFunction(f, name, fn->line);
    BasicBlock* entry = BasicBlock::Create(*ctx_, "entry", f);
    builder_->SetInsertPoint(entry);
    setDebugLoc(fn->line, fn->column);

    unsigned idx = 0;
    if (!className.empty()) {
        Argument* self = f->getArg(idx++);
        self->setName("self");
        AllocaInst* a = createEntryAlloca(f, self->getType(), "self.addr");
        builder_->CreateStore(self, a);
        locals_["self"] = a;
    }
    for (auto& p : fn->params) {
        Argument* arg = f->getArg(idx++);
        std::string pname = p->name.empty() ? "_arg" : p->name;
        arg->setName(pname);
        AllocaInst* a = createEntryAlloca(f, arg->getType(), pname);
        builder_->CreateStore(arg, a);
        locals_[pname] = a;
    }

    inMethod_ = !className.empty();
    currentClass_ = className;
    emitBlock(fn->body.get());
    inMethod_ = false;
    currentClass_.clear();

    if (!builder_->GetInsertBlock()->getTerminator()) {
        if (retTy->isVoidTy()) builder_->CreateRetVoid();
        else builder_->CreateRet(Constant::getNullValue(retTy));
    }
    currentFn_ = nullptr;
    currentSP_ = nullptr;
}

void Codegen::emitConstructor(AstNode* ctor, const std::string& className) {
    auto clsIt = classes_.find(className);
    if (clsIt == classes_.end()) return;
    Type* clsPtr = PointerType::getUnqual(clsIt->second);

    std::vector<Type*> params;
    for (auto& p : ctor->params) params.push_back(mapType(p->type.get()));
    FunctionType* fty = FunctionType::get(clsPtr, params, false);
    Function* f = Function::Create(fty, Function::ExternalLinkage, className + "_create", module_);

    if (!ctor->body) return;

    currentFn_ = f;
    locals_.clear();
    currentSP_ = createDIFunction(f, className + "_create", ctor->line);
    BasicBlock* entry = BasicBlock::Create(*ctx_, "entry", f);
    builder_->SetInsertPoint(entry);
    setDebugLoc(ctor->line, ctor->column);

    // malloc
    FunctionCallee mallocFn = module_->getOrInsertFunction(
        "s_gc_alloc", FunctionType::get(PointerType::getUnqual(*ctx_), {Type::getInt64Ty(*ctx_)}, false));
    Value* raw = builder_->CreateCall(mallocFn, {ConstantInt::get(Type::getInt64Ty(*ctx_), 64)});
    {
        Type* i8p = PointerType::getUnqual(*ctx_);
        FunctionCallee setfin = module_->getOrInsertFunction(
            "s_gc_set_finalizer",
            FunctionType::get(Type::getVoidTy(*ctx_), {i8p, i8p}, false));
        Function* dtor = module_->getFunction(className + "_destroy");
        if (dtor) {
            Value* fp = builder_->CreateBitCast(dtor, i8p);
            builder_->CreateCall(setfin, {raw, fp});
        }
    }
    Value* self = builder_->CreateBitCast(raw, clsPtr, "self");
    if (vtables_.count(className)) {
        Value* vtp = builder_->CreateStructGEP(clsIt->second, self, 0, "vptr");
        Value* decay = builder_->CreateBitCast(vtables_[className], PointerType::getUnqual(*ctx_), "vtdecay");
        builder_->CreateStore(decay, vtp);
    }
    AllocaInst* selfAddr = createEntryAlloca(f, clsPtr, "self.addr");
    builder_->CreateStore(self, selfAddr);
    locals_["self"] = selfAddr;
    {
        Type* i8p = PointerType::getUnqual(*ctx_);
        FunctionCallee setcn = module_->getOrInsertFunction(
            "s_set_classname",
            FunctionType::get(Type::getVoidTy(*ctx_), {i8p, i8p}, false));
        builder_->CreateCall(setcn, {self, builder_->CreateGlobalString(className, "cname")});
        FunctionCallee regm = module_->getOrInsertFunction(
            "s_reg_member",
            FunctionType::get(Type::getVoidTy(*ctx_), {i8p, i8p, Type::getInt32Ty(*ctx_)}, false));
        auto mit = memberRegs_.find(className);
        if (mit != memberRegs_.end()) {
            for (auto& pr : mit->second)
                builder_->CreateCall(regm, {
                    builder_->CreateGlobalString(className, "mcls"),
                    builder_->CreateGlobalString(pr.first, "mmem"),
                    ConstantInt::get(Type::getInt32Ty(*ctx_), pr.second)});
        }
        FunctionCallee regb = module_->getOrInsertFunction(
            "s_register_base",
            FunctionType::get(Type::getVoidTy(*ctx_), {i8p, i8p}, false));
        auto rit = classAst_.find(className);
        if (rit != classAst_.end()) {
            for (auto& b : rit->second->bases)
                builder_->CreateCall(regb, {
                    builder_->CreateGlobalString(className, "cn"),
                    builder_->CreateGlobalString(b, "bn")});
        }
    }

    /* Default-construct every base (0-arg init). super is only for specific calls. */
    auto ait = classAst_.find(className);
    if (ait != classAst_.end()) {
        for (auto& b : ait->second->bases) {
            std::string iname = b + "_init";
            Function* bf = module_->getFunction(iname);
            if (bf && bf->arg_size() == 1)
                builder_->CreateCall(bf, {self});
        }
    }

    unsigned idx = 0;
    for (auto& p : ctor->params) {
        Argument* arg = f->getArg(idx++);
        std::string pname = p->name.empty() ? "_arg" : p->name;
        arg->setName(pname);
        AllocaInst* a = createEntryAlloca(f, arg->getType(), pname);
        builder_->CreateStore(arg, a);
        locals_[pname] = a;
    }

    inMethod_ = true;
    currentClass_ = className;
    emitBlock(ctor->body.get());
    inMethod_ = false;
    currentClass_.clear();

    builder_->CreateRet(self);
    currentFn_ = nullptr;
    currentSP_ = nullptr;

    /* Class_init(self, args) — body only, used by super Base.create() */
    std::vector<Type*> iparams;
    iparams.push_back(clsPtr);
    for (auto& p : ctor->params) iparams.push_back(mapType(p->type.get()));
    FunctionType* ity = FunctionType::get(Type::getVoidTy(*ctx_), iparams, false);
    Function* init = Function::Create(ity, Function::ExternalLinkage, className + "_init", module_);
    currentFn_ = init;
    locals_.clear();
    BasicBlock* ient = BasicBlock::Create(*ctx_, "entry", init);
    builder_->SetInsertPoint(ient);
    Argument* iself = init->getArg(0);
    iself->setName("self");
    AllocaInst* isa = createEntryAlloca(init, clsPtr, "self.addr");
    builder_->CreateStore(iself, isa);
    locals_["self"] = isa;
    auto ait2 = classAst_.find(className);
    if (ait2 != classAst_.end()) {
        for (auto& b : ait2->second->bases) {
            std::string iname = b + "_init";
            Function* bf = module_->getFunction(iname);
            if (bf && bf->arg_size() == 1 && bf != init)
                builder_->CreateCall(bf, {iself});
        }
    }
    unsigned iidx = 1;
    for (auto& p : ctor->params) {
        Argument* arg = init->getArg(iidx++);
        std::string pname = p->name.empty() ? "_arg" : p->name;
        AllocaInst* a = createEntryAlloca(init, arg->getType(), pname);
        builder_->CreateStore(arg, a);
        locals_[pname] = a;
    }
    inMethod_ = true;
    currentClass_ = className;
    if (ctor->body) emitBlock(ctor->body.get());
    inMethod_ = false;
    currentClass_.clear();
    if (!builder_->GetInsertBlock()->getTerminator())
        builder_->CreateRetVoid();
    currentFn_ = nullptr;
}


void Codegen::emitDestructor(AstNode* dtor, const std::string& className) {
    auto clsIt = classes_.find(className);
    if (clsIt == classes_.end()) return;
    Type* clsPtr = PointerType::getUnqual(clsIt->second);
    FunctionType* fty = FunctionType::get(Type::getVoidTy(*ctx_), {clsPtr}, false);
    Function* f = Function::Create(fty, Function::ExternalLinkage, className + "_destroy", module_);
    if (!dtor->body) return;
    currentFn_ = f;
    locals_.clear();
    currentSP_ = createDIFunction(f, className + "_destroy", dtor->line);
    BasicBlock* entry = BasicBlock::Create(*ctx_, "entry", f);
    builder_->SetInsertPoint(entry);
    setDebugLoc(dtor->line, dtor->column);
    Argument* self = f->getArg(0);
    self->setName("self");
    AllocaInst* selfAddr = createEntryAlloca(f, clsPtr, "self.addr");
    builder_->CreateStore(self, selfAddr);
    locals_["self"] = selfAddr;
    inMethod_ = true;
    currentClass_ = className;
    emitBlock(dtor->body.get());
    inMethod_ = false;
    currentClass_.clear();
    if (!builder_->GetInsertBlock()->getTerminator())
        builder_->CreateRetVoid();
    currentFn_ = nullptr;
}

void Codegen::collectClassAst(AstNode* node) {
    if (!node) return;
    if (node->kind == AstKind::Class)
        classAst_[node->name] = node;
    for (auto& c : node->children) collectClassAst(c.get());
    if (node->start) collectClassAst(node->start.get());
}

unsigned Codegen::layoutClassFields(AstNode* cls, std::vector<Type*>& elems,
                                    std::unordered_map<std::string, unsigned>& indices) {
    // Multiple inheritance layout (C++-like, non-virtual):
    //   [0] header ptr
    //   then fields of base0 (and its bases, depth-first left-to-right)
    //   then fields of base1 ...
    //   then own fields
    // Each field name maps to its struct index; first occurrence wins on diamonds.
    if (!cls) return (unsigned)elems.size();

    for (auto& baseName : cls->bases) {
        auto it = classAst_.find(baseName);
        if (it == classAst_.end()) continue;
        layoutClassFields(it->second, elems, indices);
    }
    for (auto& m : cls->children) {
        if (m->kind != AstKind::DataDecl) continue;
        if (indices.count(m->name)) continue; // diamond / already from base
        unsigned idx = (unsigned)elems.size();
        indices[m->name] = idx;
        elems.push_back(mapType(m->type.get()));
    }
    return (unsigned)elems.size();
}

void Codegen::emitClass(AstNode* cls) {
    if (classes_.count(cls->name)) return; // already emitted

    // Emit bases first so their struct types exist
    for (auto& baseName : cls->bases) {
        auto it = classAst_.find(baseName);
        if (it != classAst_.end())
            emitClass(it->second);
    }

    std::vector<Type*> elems;
    elems.push_back(PointerType::getUnqual(*ctx_)); // [0] header
    std::unordered_map<std::string, unsigned> indices;
    layoutClassFields(cls, elems, indices);
    fieldIndex_[cls->name] = indices;

    StructType* sty = StructType::create(*ctx_, elems, cls->name);
    classes_[cls->name] = sty;

    {
        Type* i8p = PointerType::getUnqual(*ctx_);
        FunctionCallee reg = module_->getOrInsertFunction(
            "s_register_base",
            FunctionType::get(Type::getVoidTy(*ctx_), {i8p, i8p}, false));
        /* called at runtime from first ctor; also emit a global ctor snippet via create */
        (void)reg;
    }
    {
        Type* i8p = PointerType::getUnqual(*ctx_);
        FunctionCallee regm = module_->getOrInsertFunction(
            "s_reg_member",
            FunctionType::get(Type::getVoidTy(*ctx_), {i8p, i8p, Type::getInt32Ty(*ctx_)}, false));
        /* registration executed at first create; also emit calls from copy? do it in create via a one-time
           plus here we inject into each constructor start... register now via global ctor not available.
           Store and emit at start of Class_create by appending after emitConstructor. */
        (void)regm;
        for (auto& m : cls->children) {
            if (!m) continue;
            if (m->kind != AstKind::DataDecl && m->kind != AstKind::Function) continue;
            int vis = 2;
            if (m->visibility == Visibility::Public) vis = 0;
            else if (m->visibility == Visibility::Protected) vis = 1;
            /* keep list on node text? use a side table */
        }
    }
    for (auto& m : cls->children) {
        if (m && (m->kind == AstKind::DataDecl || m->kind == AstKind::Function)) {
            int vis = 2;
            if (m->visibility == Visibility::Public) vis = 0;
            else if (m->visibility == Visibility::Protected) vis = 1;
            memberRegs_[cls->name].push_back({m->name, vis});
        }
        if (m->kind == AstKind::Destructor) emitDestructor(m.get(), cls->name);
    }
    for (auto& m : cls->children) {
        if (!m) continue;
        if (m->kind == AstKind::Function) emitFunction(m.get(), cls->name);
    }

    /* vtable: inherit base virtual slots, override same names */
    std::vector<std::string> slots;
    std::vector<Constant*> fns;
    auto addVirt = [&](const std::string& mname, Constant* fptr) {
        for (size_t i = 0; i < slots.size(); ++i) {
            if (slots[i] == mname) { fns[i] = fptr; virtIndex_[cls->name][mname] = (unsigned)i; return; }
        }
        virtIndex_[cls->name][mname] = (unsigned)slots.size();
        slots.push_back(mname);
        fns.push_back(fptr);
    };
    for (auto& b : cls->bases) {
        auto bit = virtIndex_.find(b);
        if (bit == virtIndex_.end()) continue;
        for (auto& kv : bit->second) {
            std::string fname = b + "_" + kv.first;
            Function* f = module_->getFunction(fname);
            if (!f) f = module_->getFunction(cls->name + "_" + kv.first);
            if (f) addVirt(kv.first, f);
        }
    }
    for (auto& m : cls->children) {
        if (m && m->kind == AstKind::Function && m->isVirtual) {
            std::string fname = !m->mangledName.empty() ? m->mangledName : (cls->name + "_" + m->name);
            Function* f = module_->getFunction(fname);
            if (f) addVirt(m->name, f);
        }
    }
    if (!fns.empty()) {
        ArrayType* at = ArrayType::get(PointerType::getUnqual(*ctx_), fns.size());
        Constant* init = ConstantArray::get(at, fns);
        auto* gv = new GlobalVariable(*module_, at, true, GlobalValue::ExternalLinkage,
                                      init, cls->name + "_vtable");
        vtables_[cls->name] = gv;
    }
    for (auto& m : cls->children) {
        if (m && m->kind == AstKind::Constructor) emitConstructor(m.get(), cls->name);
    }

    /* memberwise copy ctor / assign */
    {
        Type* i8p = PointerType::getUnqual(*ctx_);
        FunctionType* cty = FunctionType::get(i8p, {i8p}, false);
        Function* cpy = Function::Create(cty, Function::ExternalLinkage, cls->name + "_copy", module_);
        BasicBlock* ce = BasicBlock::Create(*ctx_, "entry", cpy);
        builder_->SetInsertPoint(ce);
        FunctionCallee mallocFn = module_->getOrInsertFunction(
            "s_gc_alloc", FunctionType::get(i8p, {Type::getInt64Ty(*ctx_)}, false));
        Value* raw = builder_->CreateCall(mallocFn, {ConstantInt::get(Type::getInt64Ty(*ctx_), 128)});
        Value* dst = builder_->CreateBitCast(raw, PointerType::getUnqual(sty), "dst");
        Value* src = cpy->getArg(0);
        auto& idx = fieldIndex_[cls->name];
        for (auto& kv : idx) {
            unsigned i = kv.second;
            if (i == 0) continue;
            Type* ft = sty->getElementType(i);
            Value* sv = builder_->CreateLoad(ft, builder_->CreateStructGEP(sty, src, i));
            builder_->CreateStore(sv, builder_->CreateStructGEP(sty, dst, i));
        }
        FunctionCallee setcn = module_->getOrInsertFunction(
            "s_set_classname", FunctionType::get(Type::getVoidTy(*ctx_), {i8p, i8p}, false));
        builder_->CreateCall(setcn, {dst, builder_->CreateGlobalString(cls->name, "ccn")});
        if (vtables_.count(cls->name)) {
            Value* vtp = builder_->CreateStructGEP(sty, dst, 0, "vptr");
            builder_->CreateStore(vtables_[cls->name], vtp);
        }
        builder_->CreateRet(dst);

        FunctionType* aty = FunctionType::get(Type::getVoidTy(*ctx_), {i8p, i8p}, false);
        Function* asg = Function::Create(aty, Function::ExternalLinkage, cls->name + "_assign", module_);
        BasicBlock* ae = BasicBlock::Create(*ctx_, "entry", asg);
        builder_->SetInsertPoint(ae);
        Value* adst = asg->getArg(0);
        Value* asrc = asg->getArg(1);
        for (auto& kv : idx) {
            unsigned i = kv.second;
            if (i == 0) continue;
            Type* ft = sty->getElementType(i);
            Value* sv = builder_->CreateLoad(ft, builder_->CreateStructGEP(sty, asrc, i));
            builder_->CreateStore(sv, builder_->CreateStructGEP(sty, adst, i));
        }
        builder_->CreateRetVoid();
    }

}

void Codegen::emitNamespace(AstNode* ns) {
    for (auto& c : ns->children) {
        if (!c) continue;
        if (c->kind == AstKind::Class && c->nameParts.empty()) emitClass(c.get());
        else if (c->kind == AstKind::Namespace) emitNamespace(c.get());
        else if (c->kind == AstKind::Function) emitFunction(c.get(), "");
    }
}

void Codegen::emitStart(AstNode* start) {
    FunctionType* mainTy = FunctionType::get(
        Type::getInt32Ty(*ctx_),
        {Type::getInt32Ty(*ctx_), PointerType::getUnqual(PointerType::getUnqual(*ctx_))},
        false);
    Function* mainFn = Function::Create(mainTy, Function::ExternalLinkage, "main", module_);
    currentFn_ = mainFn;
    locals_.clear();
    int mainLine = (start && start->line > 0) ? start->line : 1;
    currentSP_ = createDIFunction(mainFn, "main", mainLine);
    BasicBlock* entry = BasicBlock::Create(*ctx_, "entry", mainFn);
    builder_->SetInsertPoint(entry);
    setDebugLoc(mainLine, 1);
    if (start && start->body) emitBlock(start->body.get());
    if (!builder_->GetInsertBlock()->getTerminator())
        builder_->CreateRet(ConstantInt::get(Type::getInt32Ty(*ctx_), 0));
    currentFn_ = nullptr;
    currentSP_ = nullptr;
}

int Codegen::emit(AstNode* module) {
    if (!module) return 1;
    collectClassAst(module);
    for (auto& item : module->children) {
        if (!item) continue;
        if (item->kind == AstKind::Namespace) emitNamespace(item.get());
        else if (item->kind == AstKind::Class && item->nameParts.empty()) emitClass(item.get());
        else if (item->kind == AstKind::Function && item->nameParts.empty()) emitFunction(item.get(), "");
    }
    if (module->start) emitStart(module->start.get());
    else {
        // Still emit empty main so we always have an entry point
        AstNode dummy(AstKind::Start);
        emitStart(&dummy);
    }

    if (dbg_) dbg_->finalize();

    std::string err;
    raw_string_ostream os(err);
    if (verifyModule(*module_, &os)) {
        std::fprintf(stderr, "LLVM verify error:\n%s\n", err.c_str());
        return 1;
    }
    return 0;
}

int Codegen::writeIR(const std::string& path) {
    std::error_code ec;
    raw_fd_ostream os(path, ec, sys::fs::OF_None);
    if (ec) {
        std::fprintf(stderr, "cannot open %s: %s\n", path.c_str(), ec.message().c_str());
        return 1;
    }
    module_->print(os, nullptr);
    return 0;
}

int Codegen::writeObject(const std::string& path) {
    InitializeAllTargetInfos();
    InitializeAllTargets();
    InitializeAllTargetMCs();
    InitializeAllAsmParsers();
    InitializeAllAsmPrinters();

    // Target triple – API differs across LLVM versions:
    //   older: setTargetTriple(StringRef)
    //   LLVM 21+: setTargetTriple(const Triple&)
    std::string tripleStr = sys::getDefaultTargetTriple();
    Triple TheTriple(tripleStr);
#if LLVM_VERSION_MAJOR >= 21
    module_->setTargetTriple(TheTriple);
#else
    module_->setTargetTriple(tripleStr);
#endif

    std::string err;
    // lookupTarget accepts StringRef on all recent versions
    const Target* target = TargetRegistry::lookupTarget(tripleStr, err);
    if (!target) {
        // Some LLVM builds prefer Triple overload
        target = TargetRegistry::lookupTarget(TheTriple.getTriple(), err);
    }
    if (!target) {
        std::fprintf(stderr, "target lookup failed: %s\n", err.c_str());
        return 1;
    }

    TargetOptions opt;
    auto rm = std::optional<Reloc::Model>(Reloc::PIC_);
    // createTargetMachine: LLVM 22+ wants Triple; older wants StringRef
#if LLVM_VERSION_MAJOR >= 22
    TargetMachine* tm = target->createTargetMachine(
        TheTriple, "generic", "", opt, rm);
#elif LLVM_VERSION_MAJOR >= 21
    TargetMachine* tm = target->createTargetMachine(
        TheTriple, "generic", "", opt, rm);
#else
    TargetMachine* tm = target->createTargetMachine(
        tripleStr, "generic", "", opt, rm);
#endif
    if (!tm) {
        std::fprintf(stderr, "could not create TargetMachine\n");
        return 1;
    }
    module_->setDataLayout(tm->createDataLayout());

    std::error_code ec;
    raw_fd_ostream dest(path, ec, sys::fs::OF_None);
    if (ec) {
        std::fprintf(stderr, "cannot open %s: %s\n", path.c_str(), ec.message().c_str());
        return 1;
    }

    // Object emission still uses the legacy pass manager API in LLVM's TargetMachine.
    // (New PassBuilder path is more involved; suppress deprecation above.)
    legacy::PassManager pass;
#if LLVM_VERSION_MAJOR >= 18
    CodeGenFileType fileType = CodeGenFileType::ObjectFile;
#else
    CodeGenFileType fileType = CGFT_ObjectFile;
#endif
    if (tm->addPassesToEmitFile(pass, dest, nullptr, fileType)) {
        std::fprintf(stderr, "TargetMachine cannot emit object file\n");
        return 1;
    }
    pass.run(*module_);
    dest.flush();
    return 0;
}

int Codegen::compileInlineC(std::string& outObj) {
    if (inlineSnippets_.empty()) { outObj.clear(); return 0; }
    outObj = "/tmp/simple_inline.c";
    FILE* f = std::fopen(outObj.c_str(), "w");
    if (!f) return 1;
    std::fprintf(f,
        "#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n"
        "#include <math.h>\n");
    for (auto& sn : inlineSnippets_) {
        std::fprintf(f, "void %s(", sn.name.c_str());
        for (size_t i = 0; i < sn.params.size(); ++i) {
            if (i) std::fprintf(f, ", ");
            const char* ty = "long long *";
            if (sn.kinds[i] == 'f') ty = "double *";
            if (sn.kinds[i] == 'p') ty = "char **";
            std::fprintf(f, "%s _s_%s", ty, sn.params[i].c_str());
        }
        if (sn.params.empty()) std::fprintf(f, "void");
        std::fprintf(f, ") {\n");
        for (auto& pn : sn.params)
            std::fprintf(f, "#define %s (*_s_%s)\n", pn.c_str(), pn.c_str());
        std::fprintf(f, "%s\n", sn.body.c_str());
        for (auto& pn : sn.params)
            std::fprintf(f, "#undef %s\n", pn.c_str());
        std::fprintf(f, "}\n\n");
    }
    std::fclose(f);
    std::string obj = "/tmp/simple_inline.o";
    std::string cmd = std::string("cc -O2 -fPIC -g -c ") + outObj + " -o " + obj;
    if (std::system(cmd.c_str()) != 0) {
        std::fprintf(stderr, "inline C compile failed\n");
        return 1;
    }
    outObj = obj;
    return 0;
}

int Codegen::compileExecutable(const std::string& path) {
    std::string objPath = path + ".o";
    if (writeObject(objPath)) return 1;
    std::string inlineObj;
    if (compileInlineC(inlineObj)) return 1;

    std::string extras;
    for (auto& e : extraObjs_)
        extras += " "" + e + """;

    // Link with C compiler + Simple runtime; -g keeps DWARF for GDB
    int rc = 1;
    const char* rt_objs[] = {
        "runtime/simple_rt.o",
        "build/runtime/simple_rt.o",
        "./build/runtime/simple_rt.o",
        "simple_rt.o",
        "/tmp/simple_rt.o",
        nullptr
    };
    for (int i = 0; rt_objs[i]; ++i) {
        std::string cmd = std::string("cc -g -fPIE -pie -o \"") + path + "\" \"" + objPath +
                          "\" " + (inlineObj.empty() ? "" : ("\"" + inlineObj + "\" ")) +
                          rt_objs[i] + " -lm 2>/dev/null";
        rc = std::system(cmd.c_str());
        if (rc == 0) break;
    }
    if (rc != 0) {
        std::string cmd =
            "cc -O2 -fPIC -g -c runtime/simple_rt.c -o /tmp/simple_rt.o 2>/dev/null || "
            "cc -O2 -fPIC -g -c simple_rt.c -o /tmp/simple_rt.o; "
            "cc -g -fPIE -pie -o \"" + path + "\" \"" + objPath + "\" " +
            (inlineObj.empty() ? std::string() : (inlineObj + " ")) + extras +
            " /tmp/simple_rt.o -lm";
        rc = std::system(cmd.c_str());
    }
    if (rc != 0) {
        std::fprintf(stderr, "link failed\n");
        return 1;
    }
    return 0;
}

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif


int Codegen::genAllocSlot(const std::string& name) {
    auto it = genSlots_.find(name);
    if (it != genSlots_.end()) return it->second;
    int sl = genNextSlot_++;
    genSlots_[name] = sl;
    return sl;
}

llvm::Value* Codegen::genGetInt(int slot) {
    Type* i8p = PointerType::getUnqual(*ctx_);
    FunctionCallee get = module_->getOrInsertFunction(
        "s_gen_get_int",
        FunctionType::get(Type::getInt64Ty(*ctx_), {i8p, Type::getInt32Ty(*ctx_)}, false));
    return builder_->CreateCall(get, {genPtr_, ConstantInt::get(Type::getInt32Ty(*ctx_), slot)}, "gslot");
}

void Codegen::genSetInt(int slot, llvm::Value* v) {
    Type* i8p = PointerType::getUnqual(*ctx_);
    FunctionCallee set = module_->getOrInsertFunction(
        "s_gen_set_int",
        FunctionType::get(Type::getVoidTy(*ctx_), {i8p, Type::getInt32Ty(*ctx_), Type::getInt64Ty(*ctx_)}, false));
    if (!v->getType()->isIntegerTy(64))
        v = builder_->CreateSExtOrTrunc(v, Type::getInt64Ty(*ctx_));
    builder_->CreateCall(set, {genPtr_, ConstantInt::get(Type::getInt32Ty(*ctx_), slot), v});
}

void Codegen::emitGenerator(AstNode* fn, const std::string& className) {
    Type* i8p = PointerType::getUnqual(*ctx_);
    Type* i32 = Type::getInt32Ty(*ctx_);
    Type* i64 = Type::getInt64Ty(*ctx_);

    std::string base = !fn->mangledName.empty() ? fn->mangledName
                     : (className.empty() ? fn->name : className + "_" + fn->name);
    std::string stepName = base + "_step";

    FunctionType* stepTy = FunctionType::get(i32, {i8p}, false);
    Function* step = Function::Create(stepTy, Function::ExternalLinkage, stepName, module_);

    // Factory: params -> SGen*
    std::vector<Type*> fparams;
    for (auto& p : fn->params) fparams.push_back(mapType(p->type.get()));
    FunctionType* facTy = FunctionType::get(i8p, fparams, false);
    Function* fac = Function::Create(facTy, Function::ExternalLinkage, base, module_);
    BasicBlock* fent = BasicBlock::Create(*ctx_, "entry", fac);
    builder_->SetInsertPoint(fent);
    FunctionCallee create = module_->getOrInsertFunction(
        "s_gen_create", FunctionType::get(i8p, {i8p}, false));
    Value* g = builder_->CreateCall(create, {step}, "gen");
    FunctionCallee setI = module_->getOrInsertFunction(
        "s_gen_set_int", FunctionType::get(Type::getVoidTy(*ctx_), {i8p, i32, i64}, false));
    unsigned ai = 0;
    int slot = 0;
    for (auto& p : fn->params) {
        Value* arg = fac->getArg(ai++);
        Value* iv = arg;
        if (!iv->getType()->isIntegerTy(64)) {
            if (iv->getType()->isIntegerTy())
                iv = builder_->CreateSExtOrTrunc(iv, i64);
            else
                iv = ConstantInt::get(i64, 0);
        }
        builder_->CreateCall(setI, {g, ConstantInt::get(i32, slot), iv});
        ++slot;
    }
    builder_->CreateRet(g);

    // Step function
    currentFn_ = step;
    locals_.clear();
    genSlots_.clear();
    genNextSlot_ = 0;
    genYieldId_ = 0;
    inGenerator_ = true;
    genYieldTok_ = (fn->type ? fn->type->typeTok : TokenKind::Int);
    if (genYieldTok_ == TokenKind::Eof || genYieldTok_ == TokenKind::Nothing)
        genYieldTok_ = TokenKind::Int;
    currentClass_ = className;
    inMethod_ = !className.empty();

    slot = 0;
    for (auto& p : fn->params) {
        std::string pname = p->name.empty() ? "_arg" : p->name;
        genSlots_[pname] = slot++;
    }
    genNextSlot_ = slot;

    BasicBlock* entry = BasicBlock::Create(*ctx_, "entry", step);
    BasicBlock* body0 = BasicBlock::Create(*ctx_, "gen.body", step);
    genDoneBB_ = BasicBlock::Create(*ctx_, "gen.done", step);
    builder_->SetInsertPoint(entry);
    genPtr_ = step->getArg(0);
    genPtr_->setName("gen");
    FunctionCallee getSt = module_->getOrInsertFunction(
        "s_gen_get_state", FunctionType::get(i32, {i8p}, false));
    Value* st = builder_->CreateCall(getSt, {genPtr_}, "state");
    genSwitch_ = builder_->CreateSwitch(st, genDoneBB_, 8);
    genSwitch_->addCase(cast<ConstantInt>(ConstantInt::get(i32, 0)), body0);

    builder_->SetInsertPoint(body0);
    if (fn->body) emitBlock(fn->body.get());
    emitBrIfOpen(genDoneBB_);

    builder_->SetInsertPoint(genDoneBB_);
    FunctionCallee setDone = module_->getOrInsertFunction(
        "s_gen_set_done", FunctionType::get(Type::getVoidTy(*ctx_), {i8p}, false));
    builder_->CreateCall(setDone, {genPtr_});
    builder_->CreateRet(ConstantInt::get(i32, 0));

    inGenerator_ = false;
    genPtr_ = nullptr;
    genSwitch_ = nullptr;
    genDoneBB_ = nullptr;
    currentFn_ = nullptr;
    currentClass_.clear();
    inMethod_ = false;
}


void Codegen::emitAccessCheck(const std::string& owner, const std::string& member) {
    Type* i8p = PointerType::getUnqual(*ctx_);
    FunctionCallee chk = module_->getOrInsertFunction(
        "s_access_or_die",
        FunctionType::get(Type::getVoidTy(*ctx_), {i8p, i8p, i8p}, false));
    std::string acc = currentClass_;
    builder_->CreateCall(chk, {
        builder_->CreateGlobalString(acc, "acc"),
        builder_->CreateGlobalString(owner, "own"),
        builder_->CreateGlobalString(member, "mem")});
}
