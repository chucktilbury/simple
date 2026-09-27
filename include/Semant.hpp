#pragma once
#include "Ast.hpp"
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>

enum class TypeKind { Void, Int, UInt, Float, Bool, String, List, Dict, Class, Null, Error, Unknown };

struct Type {
    TypeKind kind = TypeKind::Unknown;
    std::string className;
    static Type Void()   { return {TypeKind::Void,{}}; }
    static Type Int()    { return {TypeKind::Int,{}}; }
    static Type Bool()   { return {TypeKind::Bool,{}}; }
    static Type String() { return {TypeKind::String,{}}; }
    static Type Class(std::string n) { return {TypeKind::Class, std::move(n)}; }
    static Type Null()   { return {TypeKind::Null,{}}; }
    static Type Error()  { return {TypeKind::Error,{}}; }
};

enum class SymKind { Var, Field, Method, Ctor, Dtor, Class, Func };

struct Symbol {
    std::string name;
    SymKind kind = SymKind::Var;
    Type type;
    Visibility visibility = Visibility::None;
    std::string ownerClass;
};

class Semant {
public:
    int analyze(AstNode* module);
    int errorCount() const { return errors_; }

private:
    int errors_ = 0;
    struct Scope {
        std::unordered_map<std::string, Symbol> table;
        std::string className;
        Scope* parent = nullptr;
    };
    Scope* scope_ = nullptr;
    std::vector<std::unique_ptr<Scope>> scopes_;
    std::unordered_map<std::string, AstNode*> classes_;

    void pushScope(const std::string& className = {});
    void popScope();
    Symbol* lookup(const std::string& name);
    Symbol* define(const std::string& name, SymKind k, Type t, Visibility v, const std::string& owner = {});

    void error(AstNode* n, const std::string& msg);
    Type typeFromAst(AstNode* ts);
    Type analyzeExpr(AstNode* e);
    void analyzeStmt(AstNode* s);
    void analyzeBlock(AstNode* body);
    void analyzeClass(AstNode* cls);
    void analyzeNamespace(AstNode* ns);
    bool accessible(const Symbol& sym, const std::string& fromClass) const;
};
