#pragma once
#include <string>
#include <string_view>

enum class TokenKind {
    Eof,
    // Keywords
    Start, Include, Import, As, Is, Copy, Null, Namespace, Class,
    Public, Private, Protected,
    Create, Destroy, Super, Virtual,
    Integer, Int, Boolean, Bool, String, Dict, List, Unsigned, Float, Nothing,
    Type, Inline, Raise, Print, Break, Continue, Yield, Return,
    If, Else, For, While, Do, Try, Except, Finally, Exit,
    And, Or, Not, Lt, Gt, Lte, Gte, Equ, Neq,
    // Literals / id
    Identifier, LitInt, LitUns, LitFloat, LitBool, LitSStr, LitDStr, RawText,
    // Punctuation / operators
    LParen, RParen, LBrace, RBrace, LBracket, RBracket,
    Comma, Dot, Colon, Semi, Assign,
    Plus, Minus, Star, Slash, Percent, Caret,
    LtOp, GtOp, Le, Ge, Eq, Ne, AndAnd, OrOr, Bang,
    Error
};

struct Token {
    TokenKind kind = TokenKind::Eof;
    std::string lexeme;
    int line = 1;
    int column = 1;
};

inline const char* tokenName(TokenKind k) {
    switch (k) {
        case TokenKind::Eof: return "EOF";
        case TokenKind::Start: return "start";
        case TokenKind::Include: return "include";
        case TokenKind::Import: return "import";
        case TokenKind::As: return "as";
        case TokenKind::Is: return "is";
        case TokenKind::Copy: return "copy";
        case TokenKind::Null: return "null";
        case TokenKind::Namespace: return "namespace";
        case TokenKind::Class: return "class";
        case TokenKind::Public: return "public";
        case TokenKind::Private: return "private";
        case TokenKind::Protected: return "protected";
        case TokenKind::Create: return "create";
        case TokenKind::Destroy: return "destroy";
        case TokenKind::Super: return "super";
        case TokenKind::Virtual: return "virtual";
        case TokenKind::Integer: case TokenKind::Int: return "int";
        case TokenKind::Boolean: case TokenKind::Bool: return "bool";
        case TokenKind::String: return "string";
        case TokenKind::Dict: return "dict";
        case TokenKind::List: return "list";
        case TokenKind::Unsigned: return "unsigned";
        case TokenKind::Float: return "float";
        case TokenKind::Nothing: return "nothing";
        case TokenKind::Identifier: return "IDENTIFIER";
        case TokenKind::LitInt: return "INT";
        case TokenKind::LitFloat: return "FLOAT";
        case TokenKind::LitBool: return "BOOL";
        case TokenKind::LitSStr: case TokenKind::LitDStr: return "STRING";
        case TokenKind::LParen: return "(";
        case TokenKind::RParen: return ")";
        case TokenKind::LBrace: return "{";
        case TokenKind::RBrace: return "}";
        case TokenKind::Assign: return "=";
        case TokenKind::Plus: return "+";
        case TokenKind::Minus: return "-";
        case TokenKind::Star: return "*";
        case TokenKind::Slash: return "/";
        case TokenKind::Eq: return "==";
        case TokenKind::Ne: return "!=";
        default: return "?";
    }
}
