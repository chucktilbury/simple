#include "Semant.hpp"
#include <unordered_map>
#include <cstdio>
#include <sstream>
#include <algorithm>
#include <functional>

void Semant::error(AstNode* n, const std::string& msg) {
    ++errors_;
    int line = n ? n->line : 0, col = n ? n->column : 0;
    std::fprintf(stderr, "[semantic %d:%d] error: %s\n", line, col, msg.c_str());
}

void Semant::pushScope(const std::string& className) {
    auto sc = std::make_unique<Scope>();
    sc->parent = scope_;
    sc->className = className;
    scope_ = sc.get();
    scopes_.push_back(std::move(sc));
}

void Semant::popScope() {
    if (!scope_) return;
    scope_ = scope_->parent;
}

Symbol* Semant::lookup(const std::string& name) {
    for (Scope* s = scope_; s; s = s->parent) {
        auto it = s->table.find(name);
        if (it != s->table.end()) return &it->second;
    }
    return nullptr;
}

Symbol* Semant::define(const std::string& name, SymKind k, Type t, Visibility v, const std::string& owner) {
    Symbol sym;
    sym.name = name;
    sym.kind = k;
    sym.type = t;
    sym.visibility = v;
    sym.ownerClass = owner.empty() && scope_ ? scope_->className : owner;
    scope_->table[name] = sym;
    return &scope_->table[name];
}

Type Semant::typeFromAst(AstNode* ts) {
    if (!ts || ts->kind != AstKind::TypeSpec) return Type::Error();
    if (ts->isLiteralType) {
        switch (ts->typeTok) {
            case TokenKind::Integer: case TokenKind::Int: return Type::Int();
            case TokenKind::Boolean: case TokenKind::Bool: return Type::Bool();
            case TokenKind::String: return Type::String();
            case TokenKind::Nothing: return Type::Void();
            case TokenKind::Null: return Type::Null();
            case TokenKind::Float: return {TypeKind::Float,{}};
            case TokenKind::Unsigned: return {TypeKind::UInt,{}};
            case TokenKind::List: return {TypeKind::List,{}};
            case TokenKind::Dict: return {TypeKind::Dict,{}};
            default: return Type::Error();
        }
    }
    if (ts->left && ts->left->kind == AstKind::CompoundName && !ts->left->nameParts.empty()) {
        const std::string& cn = ts->left->nameParts.back();
        if (!classes_.count(cn)) {
            error(ts, "unknown type '" + cn + "'");
            return Type::Error();
        }
        return Type::Class(cn);
    }
    return Type::Error();
}

bool Semant::accessible(const Symbol& sym, const std::string& fromClass) const {
    if (sym.visibility == Visibility::Public || sym.visibility == Visibility::None)
        return true;
    if (sym.ownerClass.empty()) return true;
    if (sym.ownerClass == fromClass) return true;
    if (sym.visibility == Visibility::Protected) {
        // Walk all bases (multiple inheritance) via BFS
        std::vector<std::string> queue = {fromClass};
        std::unordered_map<std::string, bool> seen;
        for (size_t qi = 0; qi < queue.size(); ++qi) {
            const std::string& cur = queue[qi];
            if (seen[cur]) continue;
            seen[cur] = true;
            if (cur == sym.ownerClass) return true;
            auto it = classes_.find(cur);
            if (it == classes_.end()) continue;
            for (auto& b : it->second->bases)
                if (!seen[b]) queue.push_back(b);
        }
    }
    return false;
}

static std::string typeMangle(const Type& t) {
    switch (t.kind) {
        case TypeKind::Void: return "v";
        case TypeKind::Int: return "i";
        case TypeKind::UInt: return "u";
        case TypeKind::Float: return "f";
        case TypeKind::Bool: return "b";
        case TypeKind::String: return "s";
        case TypeKind::List: return "L";
        case TypeKind::Dict: return "D";
        case TypeKind::Class: return "C" + t.className;
        default: return "x";
    }
}

static std::string mangleFunction(const std::string& className, const std::string& name,
                                  const std::vector<Type>& paramTypes) {
    std::ostringstream os;
    if (!className.empty()) os << className << "_";
    os << name;
    for (auto& t : paramTypes) os << "_" << typeMangle(t);
    return os.str();
}


static std::string typeNameOf(const Type& t) {
    switch (t.kind) {
        case TypeKind::Void: return "nothing";
        case TypeKind::Int: return "int";
        case TypeKind::UInt: return "unsigned";
        case TypeKind::Float: return "float";
        case TypeKind::Bool: return "bool";
        case TypeKind::String: return "string";
        case TypeKind::List: return "list";
        case TypeKind::Dict: return "dict";
        case TypeKind::Class: return t.className.empty() ? "object" : t.className;
        case TypeKind::Null: return "null";
        case TypeKind::Error: return "error";
        default: return "unknown";
    }
}

Type Semant::analyzeExpr(AstNode* e) {
    if (!e) return Type::Error();
    switch (e->kind) {
        case AstKind::Literal:
            switch (e->litKind) {
                case TokenKind::LitInt: case TokenKind::LitUns: return Type::Int();
                case TokenKind::LitFloat: return {TypeKind::Float,{}};
                case TokenKind::LitBool: return Type::Bool();
                case TokenKind::LitSStr: case TokenKind::LitDStr: return Type::String();
                case TokenKind::Null: return Type::Null();
                default: return Type::Error();
            }
        case AstKind::Identifier: {
            Symbol* s = lookup(e->name);
            if (!s) { error(e, "undeclared '" + e->name + "'"); return Type::Error(); }
            std::string from = scope_ ? scope_->className : "";
            if ((s->kind == SymKind::Field || s->kind == SymKind::Method) && !accessible(*s, from)) {
                error(e, "cannot access private/protected member '" + e->name + "'");
                return Type::Error();
            }
            return s->type;
        }
        case AstKind::BinaryOp: {
            Type l = analyzeExpr(e->left.get());
            Type r = analyzeExpr(e->right.get());
            if (l.kind == TypeKind::Null || r.kind == TypeKind::Null) {
                if (e->op == TokenKind::Plus || e->op == TokenKind::Minus ||
                    e->op == TokenKind::Star || e->op == TokenKind::Slash ||
                    e->op == TokenKind::Percent || e->op == TokenKind::Caret) {
                    error(e, "arithmetic on null");
                    return Type::Error();
                }
            }
            if (e->op == TokenKind::Plus && l.kind == TypeKind::String && r.kind == TypeKind::String)
                return Type::String();
            if (e->op == TokenKind::Eq || e->op == TokenKind::Ne || e->op == TokenKind::LtOp ||
                e->op == TokenKind::GtOp || e->op == TokenKind::Le || e->op == TokenKind::Ge ||
                e->op == TokenKind::AndAnd || e->op == TokenKind::OrOr ||
                e->op == TokenKind::Equ || e->op == TokenKind::Neq ||
                e->op == TokenKind::Lt || e->op == TokenKind::Gt ||
                e->op == TokenKind::Lte || e->op == TokenKind::Gte ||
                e->op == TokenKind::Is)
                return Type::Bool();
            if (l.kind == TypeKind::Float || r.kind == TypeKind::Float)
                return {TypeKind::Float,{}};
            return l;
        }
        case AstKind::UnaryOp: {
            Type t = analyzeExpr(e->left.get());
            if (e->op == TokenKind::Copy && t.kind == TypeKind::Class)
                e->className = t.className;
            return t;
        }
        case AstKind::TypeCast: {
            Type target = typeFromAst(e->type.get());
            analyzeExpr(e->left.get());
            return target;
        }
        case AstKind::TypeOf: {
            Type inner = analyzeExpr(e->left.get());
            e->text = typeNameOf(inner);
            return Type::String();
        }
        case AstKind::CompoundRef: {
            if (e->children.empty()) return Type::Error();
            if (e->children.size() == 1 && e->children[0]->kind == AstKind::Identifier)
                return analyzeExpr(e->children[0].get());
            if (e->children.size() >= 2 && e->children[0]->kind == AstKind::Identifier &&
                e->children[1]->kind == AstKind::FunctionCall &&
                e->children[1]->name == "create") {
                for (auto& a : e->children[1]->args) analyzeExpr(a.get());
                return Type::Class(e->children[0]->name);
            }
            if (e->children.size() >= 2 && e->children[0]->kind == AstKind::Identifier &&
                e->children[1]->kind == AstKind::FunctionCall) {
                for (auto& a : e->children[1]->args) analyzeExpr(a.get());
                Symbol* s = lookup(e->children[1]->name);
                return s ? s->type : Type::Void();
            }
            if (e->children[0]->kind == AstKind::FunctionCall) {
                for (auto& a : e->children[0]->args) analyzeExpr(a.get());
                Symbol* s = lookup(e->children[0]->name);
                return s ? s->type : Type::Void();
            }
            if (e->children[0]->kind == AstKind::Identifier) {
                Type obj = analyzeExpr(e->children[0].get());
                for (size_t i = 1; i < e->children.size(); ++i) {
                    auto& item = e->children[i];
                    if (item->kind == AstKind::Identifier) {
                        Symbol* s = lookup(item->name);
                        if (s) {
                            std::string from = scope_ ? scope_->className : "";
                            if (!accessible(*s, from))
                                error(item.get(), "cannot access member '" + item->name + "'");
                            obj = s->type;
                        }
                    } else if (item->kind == AstKind::FunctionCall) {
                        std::vector<Type> argTys;
                        for (auto& a : item->args) argTys.push_back(analyzeExpr(a.get()));
                        std::string cls = obj.kind == TypeKind::Class ? obj.className : "";
                        item->mangledName = mangleFunction(cls, item->name, argTys);
                        Symbol* s = lookup(item->name);
                        obj = s ? s->type : Type::Void();
                    }
                }
                return obj;
            }
            return Type::Error();
        }
        case AstKind::FunctionCall: {
            std::vector<Type> argTys;
            for (auto& a : e->args) argTys.push_back(analyzeExpr(a.get()));
            if (e->text == "super") {
                std::string from = scope_ ? scope_->className : "";
                bool ok = false;
                std::vector<std::string> q = {from};
                std::unordered_map<std::string, bool> seen;
                for (size_t i = 0; i < q.size(); ++i) {
                    if (seen[q[i]]) continue;
                    seen[q[i]] = true;
                    auto it = classes_.find(q[i]);
                    if (it == classes_.end()) continue;
                    for (auto& b : it->second->bases) {
                        if (b == e->className) ok = true;
                        q.push_back(b);
                    }
                }
                if (!ok)
                    error(e, "super '" + e->className + "' is not a base of '" + from + "'");
                if (e->name == "create")
                    return Type::Class(from.empty() ? e->className : from);
                return Type::Void();
            }
            e->mangledName = mangleFunction("", e->name, argTys);
            Symbol* s = lookup(e->name);
            return s ? s->type : Type::Void();
        }
        case AstKind::FormatString: {
            for (auto& a : e->children) {
                if (a && a->left) analyzeExpr(a->left.get());
            }
            return Type::String();
        }
        case AstKind::ArrayLiteral:
            for (auto& c : e->children) analyzeExpr(c.get());
            return Type{TypeKind::List, {}};
        case AstKind::DictLiteral:
            for (auto& c : e->children) {
                if (c->left) analyzeExpr(c->left.get());
                if (c->right) analyzeExpr(c->right.get());
            }
            return Type{TypeKind::Dict, {}};
        case AstKind::ArrayRef: {
            Type base = analyzeExpr(e->left.get());
            analyzeExpr(e->right.get());
            if (base.kind == TypeKind::Dict) return Type{}; /* unknown value type */
            if (base.kind == TypeKind::String) return Type::String();
            return Type{}; /* list element type unknown statically */
        }
        default:
            return Type{};
    }
}

void Semant::analyzeStmt(AstNode* s) {
    if (!s) return;
    switch (s->kind) {
        case AstKind::Assignment: {
            Type lt = analyzeExpr(s->left.get());
            Type rt = analyzeExpr(s->right.get());
            if (lt.kind == TypeKind::Class) s->className = lt.className;
            if (rt.kind == TypeKind::Class) s->text = rt.className;
            break;
        }
        case AstKind::Return:
            if (s->left) analyzeExpr(s->left.get());
            break;
        case AstKind::If:
            if (s->condition) analyzeExpr(s->condition.get());
            analyzeBlock(s->body.get());
            if (s->elseBody) {
                if (s->elseBody->kind == AstKind::If) analyzeStmt(s->elseBody.get());
                else analyzeBlock(s->elseBody.get());
            }
            break;
        case AstKind::While:
        case AstKind::DoWhile:
            if (s->condition) analyzeExpr(s->condition.get());
            analyzeBlock(s->body.get());
            break;
        case AstKind::For: {
            if (s->condition) analyzeExpr(s->condition.get());
            if (!s->name.empty()) {
                Type t = s->type ? typeFromAst(s->type.get()) : Type::Int();
                define(s->name, SymKind::Var, t, Visibility::Public);
            }
            analyzeBlock(s->body.get());
            break;
        }
        case AstKind::Exit:
            if (s->left) analyzeExpr(s->left.get());
            break;
        case AstKind::Break:
        case AstKind::Continue:
            break;
        case AstKind::Yield:
            if (s->left) analyzeExpr(s->left.get());
            break;
        case AstKind::DataDecl: {
            Type t = typeFromAst(s->type.get());
            Visibility v = s->visibility != Visibility::None ? s->visibility : Visibility::Private;
            define(s->name, SymKind::Var, t, v);
            if (t.kind == TypeKind::Class) s->className = t.className;
            if (s->init) analyzeExpr(s->init.get());
            break;
        }
        case AstKind::Raise:
            /* left is compound name of exception type – accept any name */
            break;
        case AstKind::Print:
            if (s->left) analyzeExpr(s->left.get());
            break;
        case AstKind::Try:
            analyzeBlock(s->body.get());
            for (auto& ex : s->children) {
                if (ex->kind == AstKind::Except) {
                    if (!ex->name.empty())
                        define(ex->name, SymKind::Var, Type::String(), Visibility::Public);
                    analyzeBlock(ex->body.get());
                }
            }
            if (s->elseBody) analyzeBlock(s->elseBody.get()); /* finally */
            break;
        case AstKind::CompoundRef:
        case AstKind::FunctionCall:
            analyzeExpr(s);
            break;
        case AstKind::Body:
            analyzeBlock(s);
            break;
        default:
            break;
    }
}

void Semant::analyzeBlock(AstNode* body) {
    if (!body) return;
    for (auto& c : body->children) analyzeStmt(c.get());
}

void Semant::analyzeClass(AstNode* cls) {
    for (auto& b : cls->bases) {
        if (!classes_.count(b))
            error(cls, "unknown base class '" + b + "'");
    }
    classes_[cls->name] = cls;
    define(cls->name, SymKind::Class, Type::Class(cls->name),
           cls->visibility != Visibility::None ? cls->visibility : Visibility::Public);

    pushScope(cls->name);

    // Import members from all bases (multiple inheritance, left-to-right)
    // First definition wins on name clashes; report error on field conflicts.
    for (auto& baseName : cls->bases) {
        auto bit = classes_.find(baseName);
        if (bit == classes_.end()) continue;
        AstNode* base = bit->second;
        // Recursively, bases of base are already in base's children only;
        // also pull members that base itself inherited by walking base AST chain.
        std::vector<AstNode*> chain = {base};
        // Include linearization: base first, then its bases depth-first
        std::vector<std::string> bqueue = base->bases;
        std::unordered_map<std::string, bool> bseen;
        bseen[baseName] = true;
        for (size_t qi = 0; qi < bqueue.size(); ++qi) {
            if (bseen[bqueue[qi]]) continue;
            bseen[bqueue[qi]] = true;
            auto it2 = classes_.find(bqueue[qi]);
            if (it2 == classes_.end()) continue;
            chain.push_back(it2->second);
            for (auto& bb : it2->second->bases)
                if (!bseen[bb]) bqueue.push_back(bb);
        }
        // Define from farthest base first so nearer overrides
        for (int ci = (int)chain.size() - 1; ci >= 0; --ci) {
            AstNode* bnode = chain[ci];
            for (auto& m : bnode->children) {
                if (m->kind == AstKind::DataDecl) {
                    if (scope_->table.count(m->name)) {
                        Symbol* existing = &scope_->table[m->name];
                        if (existing->kind == SymKind::Field &&
                            existing->ownerClass != bnode->name) {
                            error(cls, "field '" + m->name +
                                  "' conflicts between bases '" +
                                  existing->ownerClass + "' and '" + bnode->name + "'");
                        }
                        continue; // nearer/already defined wins
                    }
                    Type t = typeFromAst(m->type.get());
                    define(m->name, SymKind::Field, t, m->visibility, bnode->name);
                } else if (m->kind == AstKind::Function) {
                    if (scope_->table.count(m->name)) continue;
                    Type ret = m->type ? typeFromAst(m->type.get()) : Type::Void();
                    define(m->name, SymKind::Method, ret, m->visibility, bnode->name);
                }
            }
        }
    }

    for (auto& m : cls->children) {
        Visibility v = m->visibility != Visibility::None ? m->visibility : Visibility::Private;
        if (m->kind == AstKind::DataDecl) {
            Type t = typeFromAst(m->type.get());
            define(m->name, SymKind::Field, t, v, cls->name);
        } else if (m->kind == AstKind::Function) {
            Type ret = m->type ? typeFromAst(m->type.get()) : Type::Void();
            std::vector<Type> pts;
            for (auto& p : m->params) pts.push_back(typeFromAst(p->type.get()));
            m->mangledName = mangleFunction(cls->name, m->name, pts);
            define(m->name, SymKind::Method, ret, v, cls->name);
            define(m->mangledName, SymKind::Method, ret, v, cls->name);
            if (m->body) {
                pushScope(cls->name);
                for (auto& p : m->params) {
                    Type pt = typeFromAst(p->type.get());
                    define(p->name.empty() ? "_arg" : p->name, SymKind::Var, pt, Visibility::Public);
                }
                define("self", SymKind::Var, Type::Class(cls->name), Visibility::Public);
                analyzeBlock(m->body.get());
                popScope();
            }
        } else if (m->kind == AstKind::Constructor) {
            std::vector<Type> pts;
            for (auto& p : m->params) pts.push_back(typeFromAst(p->type.get()));
            m->mangledName = mangleFunction(cls->name, "create", pts);
            define("create", SymKind::Ctor, Type::Class(cls->name), v, cls->name);
            define(m->mangledName, SymKind::Ctor, Type::Class(cls->name), v, cls->name);
            if (m->body) {
                pushScope(cls->name);
                for (auto& p : m->params) {
                    Type pt = typeFromAst(p->type.get());
                    define(p->name.empty() ? "_arg" : p->name, SymKind::Var, pt, Visibility::Public);
                }
                define("self", SymKind::Var, Type::Class(cls->name), Visibility::Public);
                analyzeBlock(m->body.get());
                popScope();
            }
        } else if (m->kind == AstKind::Destructor) {
            m->mangledName = cls->name + "_destroy";
            define("destroy", SymKind::Dtor, Type::Void(), v, cls->name);
            if (m->body) {
                pushScope(cls->name);
                define("self", SymKind::Var, Type::Class(cls->name), Visibility::Public);
                analyzeBlock(m->body.get());
                popScope();
            }
        }
    }
    popScope();
}

void Semant::analyzeNamespace(AstNode* ns) {
    pushScope();
    for (auto& c : ns->children) {
        if (c->kind == AstKind::Class) analyzeClass(c.get());
        else if (c->kind == AstKind::Namespace) analyzeNamespace(c.get());
    }
    popScope();
}


static int g_attach_errors = 0;

static void attachOutOfClassDefs(AstNode* node,
                                 std::unordered_map<std::string, AstNode*>& classes) {
    if (!node) return;
    for (auto& c : node->children) {
        if (!c) continue;
        if (c->kind == AstKind::Namespace) {
            attachOutOfClassDefs(c.get(), classes);
            continue;
        }
        if ((c->kind != AstKind::Function && c->kind != AstKind::Constructor
             && c->kind != AstKind::Destructor)
            || c->className.empty())
            continue;
        auto it = classes.find(c->className);
        if (it == classes.end()) {
            std::fprintf(stderr, "[semantic %d:%d] error: out-of-class member for unknown class '%s'\n",
                         c->line, c->column, c->className.c_str());
            continue;
        }
        AstNode* cls = it->second;
        bool attached = false;
        for (auto& m : cls->children) {
            if (!m) continue;
            bool same = (c->kind == m->kind) &&
                (c->kind != AstKind::Function || m->name == c->name);
            if (!same) continue;
            if (!m->body && c->body) m->body = std::move(c->body);
            if (m->params.empty() && !c->params.empty())
                m->params = std::move(c->params);
            attached = true;
            break;
        }
        if (!attached) {
            ++g_attach_errors;
            std::fprintf(stderr, "[semantic %d:%d] error: '%s.%s' is not declared in class '%s'\n",
                         c->line, c->column,
                         c->className.c_str(),
                         c->name.empty() ? (c->kind == AstKind::Destructor ? "destroy" : "create") : c->name.c_str(),
                         c->className.c_str());
        }
        c.reset(); /* body stolen or rejected — drop module slot */
    }
    /* drop emptied slots so later walks never see nullptr children */
    auto& ch = node->children;
    ch.erase(std::remove_if(ch.begin(), ch.end(),
                            [](const AstPtr& x){ return !x; }),
             ch.end());
}


static bool containsYield(AstNode* n) {
    if (!n) return false;
    if (n->kind == AstKind::Yield) return true;
    if (containsYield(n->left.get()) || containsYield(n->right.get()) ||
        containsYield(n->body.get()) || containsYield(n->condition.get()) ||
        containsYield(n->elseBody.get()))
        return true;
    for (auto& c : n->children) if (containsYield(c.get())) return true;
    return false;
}

static void markGenerators(AstNode* n) {
    if (!n) return;
    if (n->kind == AstKind::Function && containsYield(n->body.get()))
        n->isGenerator = true;
    for (auto& c : n->children) markGenerators(c.get());
    if (n->start) markGenerators(n->start.get());
    if (n->body) markGenerators(n->body.get());
}


static AstPtr cloneAst(AstNode* n) {
    if (!n) return nullptr;
    auto c = std::make_unique<AstNode>(n->kind, n->line, n->column);
    c->visibility = n->visibility;
    c->name = n->name;
    c->className = n->className;
    c->text = n->text;
    c->op = n->op;
    c->litKind = n->litKind;
    c->isLiteralType = n->isLiteralType;
    c->typeTok = n->typeTok;
    c->isVirtual = n->isVirtual;
    c->isOverride = n->isOverride;
    c->isStatic = n->isStatic;
    c->isGenerator = n->isGenerator;
    c->nameParts = n->nameParts;
    c->bases = n->bases;
    c->mangledName = n->mangledName;
    c->type = cloneAst(n->type.get());
    c->left = cloneAst(n->left.get());
    c->right = cloneAst(n->right.get());
    c->body = cloneAst(n->body.get());
    c->condition = cloneAst(n->condition.get());
    c->elseBody = cloneAst(n->elseBody.get());
    c->init = cloneAst(n->init.get());
    c->start = cloneAst(n->start.get());
    for (auto& x : n->children) c->children.push_back(cloneAst(x.get()));
    for (auto& x : n->params) c->params.push_back(cloneAst(x.get()));
    for (auto& x : n->args) c->args.push_back(cloneAst(x.get()));
    return c;
}

static std::string mangleTypeSpec(AstNode* ts) {
    if (!ts) return "void";
    if (ts->isLiteralType) {
        switch (ts->typeTok) {
            case TokenKind::Integer: case TokenKind::Int: return "int";
            case TokenKind::Float: return "float";
            case TokenKind::Boolean: case TokenKind::Bool: return "bool";
            case TokenKind::String: return "string";
            case TokenKind::Unsigned: return "unsigned";
            case TokenKind::List: return "list";
            case TokenKind::Dict: return "dict";
            case TokenKind::Nothing: return "nothing";
            default: return "t";
        }
    }
    std::string n;
    if (ts->left && ts->left->kind == AstKind::CompoundName && !ts->left->nameParts.empty())
        n = ts->left->nameParts.back();
    else if (!ts->text.empty()) n = ts->text;
    else n = "T";
    for (auto& a : ts->args) n += "_" + mangleTypeSpec(a.get());
    return n;
}

static void substTypes(AstNode* n, const std::unordered_map<std::string, AstNode*>& subst) {
    if (!n) return;
    auto subOne = [&](AstPtr& ts) {
        if (!ts || ts->kind != AstKind::TypeSpec) return;
        if (ts->isLiteralType) return;
        std::string nm;
        if (ts->left && ts->left->kind == AstKind::CompoundName && !ts->left->nameParts.empty())
            nm = ts->left->nameParts.back();
        auto it = subst.find(nm);
        if (it != subst.end() && it->second) {
            ts = cloneAst(it->second);
            return;
        }
        for (auto& a : ts->args) substTypes(a.get(), subst);
    };
    subOne(n->type);
    substTypes(n->left.get(), subst);
    substTypes(n->right.get(), subst);
    substTypes(n->body.get(), subst);
    substTypes(n->condition.get(), subst);
    substTypes(n->elseBody.get(), subst);
    substTypes(n->init.get(), subst);
    substTypes(n->start.get(), subst);
    for (auto& c : n->children) substTypes(c.get(), subst);
    for (auto& c : n->params) substTypes(c.get(), subst);
    for (auto& c : n->args) substTypes(c.get(), subst);
}

static void instantiateTemplates(AstNode* module) {
    if (!module) return;
    std::unordered_map<std::string, AstNode*> classTmpl;
    std::unordered_map<std::string, AstNode*> fnTmpl;
    std::function<void(AstNode*)> collect = [&](AstNode* n) {
        if (!n) return;
        if (n->kind == AstKind::Class && !n->nameParts.empty())
            classTmpl[n->name] = n;
        if (n->kind == AstKind::Function && !n->nameParts.empty() && n->className.empty())
            fnTmpl[n->name] = n;
        for (auto& c : n->children) collect(c.get());
    };
    collect(module);

    std::function<void(AstNode*)> walk;
    walk = [&](AstNode* n) {
        if (!n) return;
        if (n->kind == AstKind::TypeSpec && !n->args.empty()) {
            std::string base;
            if (n->left && n->left->kind == AstKind::CompoundName && !n->left->nameParts.empty())
                base = n->left->nameParts.back();
            else base = n->text;
            for (auto& a : n->args) walk(a.get());
            auto it = classTmpl.find(base);
            if (it != classTmpl.end()) {
                std::string iname = base;
                for (auto& a : n->args) iname += "_" + mangleTypeSpec(a.get());
                bool exists = false;
                for (auto& ch : module->children)
                    if (ch && ch->kind == AstKind::Class && ch->name == iname) exists = true;
                if (!exists) {
                    auto inst = cloneAst(it->second);
                    inst->name = iname;
                    std::unordered_map<std::string, AstNode*> sub;
                    for (size_t i = 0; i < it->second->nameParts.size() && i < n->args.size(); ++i)
                        sub[it->second->nameParts[i]] = n->args[i].get();
                    substTypes(inst.get(), sub);
                    inst->nameParts.clear();
                    module->children.push_back(std::move(inst));
                }
                n->args.clear();
                n->isLiteralType = false;
                n->text = iname;
                auto cn = std::make_unique<AstNode>(AstKind::CompoundName, n->line, n->column);
                cn->nameParts.push_back(iname);
                n->left = std::move(cn);
            }
        }
        if (n->kind == AstKind::Identifier && !n->args.empty()) {
            std::string base = n->name;
            auto it = classTmpl.find(base);
            if (it != classTmpl.end()) {
                std::string iname = base;
                for (auto& a : n->args) iname += "_" + mangleTypeSpec(a.get());
                bool exists = false;
                for (auto& ch : module->children)
                    if (ch && ch->kind == AstKind::Class && ch->name == iname) exists = true;
                if (!exists) {
                    auto inst = cloneAst(it->second);
                    inst->name = iname;
                    std::unordered_map<std::string, AstNode*> sub;
                    for (size_t i = 0; i < it->second->nameParts.size() && i < n->args.size(); ++i)
                        sub[it->second->nameParts[i]] = n->args[i].get();
                    substTypes(inst.get(), sub);
                    inst->nameParts.clear();
                    module->children.push_back(std::move(inst));
                }
                n->name = iname;
                n->args.clear();
            }
        }
        if (n->kind == AstKind::FunctionCall && !n->children.empty()) {
            auto it = fnTmpl.find(n->name);
            if (it != fnTmpl.end()) {
                std::string iname = n->name;
                for (auto& a : n->children) iname += "_" + mangleTypeSpec(a.get());
                bool exists = false;
                for (auto& ch : module->children)
                    if (ch && ch->kind == AstKind::Function && ch->name == iname) exists = true;
                if (!exists) {
                    auto inst = cloneAst(it->second);
                    inst->name = iname;
                    std::unordered_map<std::string, AstNode*> sub;
                    for (size_t i = 0; i < it->second->nameParts.size() && i < n->children.size(); ++i)
                        sub[it->second->nameParts[i]] = n->children[i].get();
                    substTypes(inst.get(), sub);
                    inst->nameParts.clear();
                    module->children.push_back(std::move(inst));
                }
                n->name = iname;
                n->children.clear();
            }
        }
        walk(n->type.get());
        walk(n->left.get());
        walk(n->right.get());
        walk(n->body.get());
        walk(n->condition.get());
        walk(n->elseBody.get());
        walk(n->init.get());
        walk(n->start.get());
        for (auto& c : n->children) walk(c.get());
        for (auto& c : n->params) walk(c.get());
        for (auto& c : n->args) walk(c.get());
    };
    walk(module);
    if (module->start) walk(module->start.get());
}

int Semant::analyze(AstNode* module) {
    if (!module) return 1;
    instantiateTemplates(module);
    g_attach_errors = 0;
    pushScope();
    for (auto& item : module->children) {
        if (!item) continue;
        if (item->kind == AstKind::Class && item->nameParts.empty())
            classes_[item->name] = item.get();
        else if (item->kind == AstKind::Namespace)
            for (auto& c : item->children)
                if (c->kind == AstKind::Class && c->nameParts.empty()) classes_[c->name] = c.get();
    }
    attachOutOfClassDefs(module, classes_);
    errors_ += g_attach_errors;
    markGenerators(module);
    for (auto& item : module->children) {
        if (!item) continue;
        if (item->kind == AstKind::Namespace) analyzeNamespace(item.get());
        else if (item->kind == AstKind::Class && item->nameParts.empty()) analyzeClass(item.get());
    }
    if (module->start && module->start->body) {
        pushScope();
        analyzeBlock(module->start->body.get());
        popScope();
    }
    popScope();
    return errors_ > 0 ? 1 : 0;
}
