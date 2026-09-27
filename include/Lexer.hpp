#pragma once
#include "Token.hpp"
#include <string>
#include <string_view>

class Lexer {
public:
    explicit Lexer(std::string_view source);

    Token next();
    Token peek();
    std::string readUntilMatchingRBrace();

private:
    std::string_view src_;
    size_t pos_ = 0;
    int line_ = 1;
    int column_ = 1;
    Token current_;
    bool hasCurrent_ = false;

    char peekChar() const;
    char advance();
    void skipWhitespaceAndComments();
    Token make(TokenKind kind, std::string lex, int line, int col);
    Token scanIdentifierOrKeyword();
    Token scanNumber();
    Token scanString(char quote);
};
