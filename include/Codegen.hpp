#pragma once
#include "Ast.hpp"
#include <string>
#include <memory>
#include <unordered_map>
#include <vector>
#include <utility>

namespace llvm {
class LLVMContext;
class Module;
class IRBuilderBase;
class Function;
class Value;
class Type;
class BasicBlock;
class AllocaInst;
class StructType;
class DIBuilder;
class DICompileUnit;
class DIFile;
class DISubprogram;
class DIScope;
class DIType;
class SwitchInst;
class GlobalVariable;
}

class Codegen {
public:
    Codegen(const std::string& moduleName, const std::string& sourcePath = "input.simple");
    ~Codegen();

    int emit(AstNode* module);
    int writeIR(const std::string& path);          // write .ll
    int writeObject(const std::string& path);      // write .o with DWARF
    int compileExecutable(const std::string& path); // .o + link → executable (-g)

    llvm::Module* getModule() { return module_; }
    void setDebugInfo(bool on) { emitDebug_ = on; }
    bool debugInfo() const { return emitDebug_; }
    void addExtraObj(const std::string& path) { extraObjs_.push_back(path); }

private:
    llvm::LLVMContext* ctx_;
    llvm::Module* module_;
    llvm::IRBuilderBase* builder_;

    /* Debug info */
    llvm::DIBuilder* dbg_ = nullptr;
    llvm::DICompileUnit* cu_ = nullptr;
    llvm::DIFile* file_ = nullptr;
    llvm::DISubprogram* currentSP_ = nullptr;
    std::string sourcePath_;
    bool emitDebug_ = true;
    std::vector<std::string> extraObjs_;

    llvm::Function* currentFn_ = nullptr;
    std::unordered_map<std::string, llvm::AllocaInst*> locals_;
    std::unordered_map<std::string, llvm::StructType*> classes_;
    /* field name -> struct element index (0 = header) */
    std::unordered_map<std::string, std::unordered_map<std::string, unsigned>> fieldIndex_;
    /* class AST nodes for inheritance layout */
    std::unordered_map<std::string, AstNode*> classAst_;
    std::unordered_map<std::string, std::unordered_map<std::string, unsigned>> virtIndex_;
    std::unordered_map<std::string, std::string> localClass_;
    std::unordered_map<std::string, llvm::GlobalVariable*> vtables_;
    std::unordered_map<std::string, std::vector<std::pair<std::string,int>>> memberRegs_;


    std::string currentClass_;
    bool inMethod_ = false;
    int errors_ = 0;

    struct LoopFrame {
        llvm::BasicBlock* cont = nullptr;
        llvm::BasicBlock* end = nullptr;
    };
    std::vector<LoopFrame> loops_;
    void emitBrIfOpen(llvm::BasicBlock* dest);

    bool inGenerator_ = false;
    llvm::Value* genPtr_ = nullptr;
    llvm::BasicBlock* genDoneBB_ = nullptr;
    llvm::SwitchInst* genSwitch_ = nullptr;
    int genYieldId_ = 0;
    int genNextSlot_ = 0;
    TokenKind genYieldTok_ = TokenKind::Int;
    std::unordered_map<std::string, int> genSlots_;
    void emitGenerator(AstNode* fn, const std::string& className);
    void emitAccessCheck(const std::string& owner, const std::string& member);

    struct InlineSnippet {
        std::string name;
        std::string body;
        std::vector<std::string> params;
        std::vector<char> kinds; /* i=i64, f=double, p=ptr */
    };
    std::vector<InlineSnippet> inlineSnippets_;

    int compileInlineC(std::string& outObj);

    llvm::Value* genGetInt(int slot);
    void genSetInt(int slot, llvm::Value* v);
    int genAllocSlot(const std::string& name);



    void setDebugLoc(int line, int col);
    llvm::DIType* diBasicType(const char* name, unsigned bits, unsigned encoding);
    llvm::DISubprogram* createDIFunction(llvm::Function* f, const std::string& name,
                                         int line, bool isLocal = false);

    llvm::Type* mapType(AstNode* ts);
    llvm::AllocaInst* createEntryAlloca(llvm::Function* fn, llvm::Type* ty, const std::string& name);
    llvm::Value* emitExpr(AstNode* e);
    void emitStmt(AstNode* s);
    void emitBlock(AstNode* body);
    void emitClass(AstNode* cls);
    void collectClassAst(AstNode* module);
    unsigned layoutClassFields(AstNode* cls, std::vector<llvm::Type*>& elems,
                              std::unordered_map<std::string, unsigned>& indices);
    void emitNamespace(AstNode* ns);
    void emitFunction(AstNode* fn, const std::string& className);
    void emitConstructor(AstNode* ctor, const std::string& className);
    void emitDestructor(AstNode* dtor, const std::string& className);
    void emitStart(AstNode* start);
};
