#pragma once
#include "Lexer.hpp"
#include "Ast.hpp"
#include <memory>
#include <string>

class Parser {
public:
    explicit Parser(Lexer& lexer);
    AstPtr parseModule();
    bool hadError() const { return hadError_; }
    int errorCount() const { return errorCount_; }

private:
    Lexer& lex_;
    Token cur_;
    bool hadError_ = false;
    bool panic_ = false;
    int errorCount_ = 0;

    void advance();
    bool check(TokenKind k) const;
    bool match(TokenKind k);
    void consume(TokenKind k, const char* msg);
    void error(const char* msg);
    bool isStmtStart() const;
    bool isDeclStart() const;
    void synchronizeStmt();
    void synchronizeDecl();

    AstPtr parseExpression();
    AstPtr parseExpressionCont(AstPtr left, int minPrec);
    AstPtr parseExpressionPrec(int minPrec);
    AstPtr parsePrimary();
    AstPtr parseTypeSpecifier();
    AstPtr parseCompoundName();
    AstPtr parseFunctionBody();
    AstPtr parseLoopBody();
    AstPtr parseBodyItem();
    AstPtr parseClass();
    AstPtr parseNamespace();
    AstPtr parseModuleItem();
    AstPtr parseOutOfClassDef();
    AstPtr parseFunction(bool isDef);
    AstPtr parseConstructor(bool isDef);
    AstPtr parseDestructor(bool isDef);
};
