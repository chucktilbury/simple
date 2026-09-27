#include "Lexer.hpp"
#include <cctype>
#include <unordered_map>

Lexer::Lexer(std::string_view source) : src_(source) {}

char Lexer::peekChar() const {
    return pos_ < src_.size() ? src_[pos_] : '\0';
}

char Lexer::advance() {
    if (pos_ >= src_.size()) return '\0';
    char c = src_[pos_++];
    if (c == '\n') { ++line_; column_ = 1; }
    else ++column_;
    return c;
}

void Lexer::skipWhitespaceAndComments() {
    for (;;) {
        char c = peekChar();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance();
        } else if (c == '#') {
            while (peekChar() && peekChar() != '\n') advance();
        } else if (c == '/' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '/') {
            advance(); advance();
            while (peekChar() && peekChar() != '\n') advance();
        } else if (c == '/' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '*') {
            advance(); advance();
            while (peekChar()) {
                if (peekChar() == '*' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '/') {
                    advance(); advance();
                    break;
                }
                advance();
            }
        } else break;
    }
}

Token Lexer::make(TokenKind kind, std::string lex, int line, int col) {
    Token t;
    t.kind = kind;
    t.lexeme = std::move(lex);
    t.line = line;
    t.column = col;
    return t;
}

Token Lexer::scanIdentifierOrKeyword() {
    int line = line_, col = column_;
    size_t start = pos_;
    while (std::isalnum(static_cast<unsigned char>(peekChar())) || peekChar() == '_')
        advance();
    std::string text(src_.substr(start, pos_ - start));

    static const std::unordered_map<std::string, TokenKind> kw = {
        {"start", TokenKind::Start}, {"include", TokenKind::Include},
        {"import", TokenKind::Import}, {"as", TokenKind::As}, {"is", TokenKind::Is}, {"copy", TokenKind::Copy}, {"null", TokenKind::Null},
        {"namespace", TokenKind::Namespace}, {"class", TokenKind::Class},
        {"public", TokenKind::Public}, {"private", TokenKind::Private},
        {"protected", TokenKind::Protected},
        {"create", TokenKind::Create}, {"destroy", TokenKind::Destroy}, {"super", TokenKind::Super}, {"virtual", TokenKind::Virtual},
        {"integer", TokenKind::Integer}, {"int", TokenKind::Int},
        {"boolean", TokenKind::Boolean}, {"bool", TokenKind::Bool},
        {"string", TokenKind::String}, {"dict", TokenKind::Dict},
        {"list", TokenKind::List}, {"unsigned", TokenKind::Unsigned},
        {"float", TokenKind::Float}, {"nothing", TokenKind::Nothing},
        {"type", TokenKind::Type}, {"inline", TokenKind::Inline},
        {"raise", TokenKind::Raise}, {"print", TokenKind::Print}, {"break", TokenKind::Break},
        {"continue", TokenKind::Continue}, {"yield", TokenKind::Yield},
        {"return", TokenKind::Return}, {"if", TokenKind::If},
        {"else", TokenKind::Else}, {"for", TokenKind::For},
        {"while", TokenKind::While}, {"do", TokenKind::Do},
        {"try", TokenKind::Try}, {"except", TokenKind::Except},
        {"finally", TokenKind::Finally}, {"exit", TokenKind::Exit},
        {"and", TokenKind::And}, {"or", TokenKind::Or}, {"not", TokenKind::Not},
        {"lt", TokenKind::Lt}, {"gt", TokenKind::Gt},
        {"lte", TokenKind::Lte}, {"gte", TokenKind::Gte},
        {"equ", TokenKind::Equ}, {"neq", TokenKind::Neq},
        {"true", TokenKind::LitBool}, {"false", TokenKind::LitBool},
    };
    auto it = kw.find(text);
    if (it != kw.end()) return make(it->second, text, line, col);
    return make(TokenKind::Identifier, text, line, col);
}

Token Lexer::scanNumber() {
    int line = line_, col = column_;
    size_t start = pos_;
    bool isFloat = false;
    while (std::isdigit(static_cast<unsigned char>(peekChar()))) advance();
    if (peekChar() == '.') {
        isFloat = true;
        advance();
        while (std::isdigit(static_cast<unsigned char>(peekChar()))) advance();
    }
    if (peekChar() == 'u' || peekChar() == 'U') { advance(); return make(TokenKind::LitUns, std::string(src_.substr(start, pos_ - start)), line, col); }
    if (peekChar() == 'f' || peekChar() == 'F') { isFloat = true; advance(); }
    return make(isFloat ? TokenKind::LitFloat : TokenKind::LitInt,
                std::string(src_.substr(start, pos_ - start)), line, col);
}

Token Lexer::scanString(char quote) {
    int line = line_, col = column_;
    size_t start = pos_;
    advance(); // opening
    while (peekChar() && peekChar() != quote) {
        if (peekChar() == '\\') { advance(); if (peekChar()) advance(); }
        else advance();
    }
    if (peekChar() != quote) return make(TokenKind::Error, "unterminated string", line, col);
    advance();
    return make(quote == '\'' ? TokenKind::LitSStr : TokenKind::LitDStr,
                std::string(src_.substr(start, pos_ - start)), line, col);
}

Token Lexer::next() {
    skipWhitespaceAndComments();
    if (pos_ >= src_.size()) return make(TokenKind::Eof, "", line_, column_);

    int line = line_, col = column_;
    char c = peekChar();
    size_t start = pos_;

    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_')
        return scanIdentifierOrKeyword();
    if (std::isdigit(static_cast<unsigned char>(c)))
        return scanNumber();
    if (c == '\'' || c == '"')
        return scanString(c);

    // Two-char ops
    if (c == '=' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '=') { advance(); advance(); return make(TokenKind::Eq, "==", line, col); }
    if (c == '!' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '=') { advance(); advance(); return make(TokenKind::Ne, "!=", line, col); }
    if (c == '<' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '=') { advance(); advance(); return make(TokenKind::Le, "<=", line, col); }
    if (c == '>' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '=') { advance(); advance(); return make(TokenKind::Ge, ">=", line, col); }
    if (c == '&' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '&') { advance(); advance(); return make(TokenKind::AndAnd, "&&", line, col); }
    if (c == '|' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '|') { advance(); advance(); return make(TokenKind::OrOr, "||", line, col); }

    advance();
    switch (c) {
        case '(': return make(TokenKind::LParen, "(", line, col);
        case ')': return make(TokenKind::RParen, ")", line, col);
        case '{': return make(TokenKind::LBrace, "{", line, col);
        case '}': return make(TokenKind::RBrace, "}", line, col);
        case '[': return make(TokenKind::LBracket, "[", line, col);
        case ']': return make(TokenKind::RBracket, "]", line, col);
        case ',': return make(TokenKind::Comma, ",", line, col);
        case '.': return make(TokenKind::Dot, ".", line, col);
        case ':': return make(TokenKind::Colon, ":", line, col);
        case ';': return make(TokenKind::Semi, ";", line, col);
        case '=': return make(TokenKind::Assign, "=", line, col);
        case '+': return make(TokenKind::Plus, "+", line, col);
        case '-': return make(TokenKind::Minus, "-", line, col);
        case '*': return make(TokenKind::Star, "*", line, col);
        case '/': return make(TokenKind::Slash, "/", line, col);
        case '%': return make(TokenKind::Percent, "%", line, col);
        case '^': return make(TokenKind::Caret, "^", line, col);
        case '<': return make(TokenKind::LtOp, "<", line, col);
        case '>': return make(TokenKind::GtOp, ">", line, col);
        case '!': return make(TokenKind::Bang, "!", line, col);
        default:  return make(TokenKind::Error, std::string(1, c), line, col);
    }
}

Token Lexer::peek() {
    if (!hasCurrent_) {
        current_ = next();
        hasCurrent_ = true;
    }
    return current_;
}


std::string Lexer::readUntilMatchingRBrace() {
    /* pos_ is just after the opening '{'. Read until matching '}'. */
    std::string out;
    int depth = 1;
    bool inStr = false, inChr = false, esc = false;
    while (pos_ < src_.size() && depth > 0) {
        char c = src_[pos_];
        if (inStr) {
            out.push_back(c);
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') inStr = false;
            advance();
            continue;
        }
        if (inChr) {
            out.push_back(c);
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '\'') inChr = false;
            advance();
            continue;
        }
        if (c == '"') { inStr = true; out.push_back(c); advance(); continue; }
        if (c == '\'') { inChr = true; out.push_back(c); advance(); continue; }
        if (c == '/' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '/') {
            while (pos_ < src_.size() && src_[pos_] != '\n') { out.push_back(src_[pos_]); advance(); }
            continue;
        }
        if (c == '{') { ++depth; out.push_back(c); advance(); continue; }
        if (c == '}') {
            --depth;
            if (depth == 0) { advance(); break; }
            out.push_back(c);
            advance();
            continue;
        }
        out.push_back(c);
        advance();
    }
    return out;
}
