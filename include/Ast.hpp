#pragma once
#include "Token.hpp"
#include <memory>
#include <string>
#include <vector>

enum class Visibility { None, Public, Private, Protected };

enum class AstKind {
    Module, Start, Include, Import, Namespace, Class,
    Function, Constructor, Destructor, DataDecl,
    TypeSpec, CompoundName, Param, Body,
    Assignment, Return, Raise, Print, Exit, Break, Continue, Yield, Inline,
    If, For, While, DoWhile, Try, Except, Finally,
    BinaryOp, UnaryOp, Literal, Identifier, CompoundRef,
    FunctionCall, ArrayRef, TypeCast, TypeOf,
    ArrayLiteral, DictLiteral, DictItem,
    FormatString, FormatArg
};

struct AstNode {
    AstKind kind;
    int line = 0, column = 0;
    Visibility visibility = Visibility::None;

    std::string name;
    std::string className;
    std::string text;
    TokenKind op = TokenKind::Eof;
    TokenKind litKind = TokenKind::Eof;
    bool isLiteralType = false;
    TokenKind typeTok = TokenKind::Eof;
    bool isVirtual = false;      /* virtual method */
    bool isOverride = false;     /* overrides base */
    bool isStatic = false;
    bool isGenerator = false;  /* contains yield */

    std::unique_ptr<AstNode> type;
    std::unique_ptr<AstNode> left;
    std::unique_ptr<AstNode> right;
    std::unique_ptr<AstNode> body;
    std::unique_ptr<AstNode> condition;
    std::unique_ptr<AstNode> elseBody;
    std::unique_ptr<AstNode> init;
    std::unique_ptr<AstNode> start;

    std::vector<std::unique_ptr<AstNode>> children;
    std::vector<std::unique_ptr<AstNode>> params;
    std::vector<std::unique_ptr<AstNode>> args;
    std::vector<std::string> nameParts;
    /* Inheritance: list of base class names (single inheritance uses [0]) */
    std::vector<std::string> bases;
    /* Overload mangled name filled by semantic analysis */
    std::string mangledName;

    explicit AstNode(AstKind k, int l = 0, int c = 0) : kind(k), line(l), column(c) {}
};

using AstPtr = std::unique_ptr<AstNode>;
