#include "Parser.hpp"
#include <cstdio>
#include <cstdlib>
#include <utility>

Parser::Parser(Lexer& lexer) : lex_(lexer) {
    advance();
}

void Parser::advance() {
    cur_ = lex_.next();
    while (cur_.kind == TokenKind::Error) {
        if (!panic_)
            error("unexpected character or unterminated string");
        Token nxt = lex_.next();
        cur_ = nxt;
        if (cur_.kind == TokenKind::Eof) break;
    }
}

bool Parser::check(TokenKind k) const { return cur_.kind == k; }

bool Parser::match(TokenKind k) {
    if (check(k)) { advance(); return true; }
    return false;
}

void Parser::error(const char* msg) {
    if (panic_) return;
    panic_ = true;
    hadError_ = true;
    ++errorCount_;
    std::fprintf(stderr, "[%d:%d] parse error: %s (got %s '%s')\n",
                 cur_.line, cur_.column, msg, tokenName(cur_.kind), cur_.lexeme.c_str());
}

void Parser::consume(TokenKind k, const char* msg) {
    if (check(k)) { advance(); return; }
    error(msg);
}

bool Parser::isStmtStart() const {
    switch (cur_.kind) {
        case TokenKind::If: case TokenKind::Else:
        case TokenKind::For: case TokenKind::While: case TokenKind::Do:
        case TokenKind::Return: case TokenKind::Raise: case TokenKind::Print:
        case TokenKind::Try: case TokenKind::Except: case TokenKind::Finally:
        case TokenKind::Break: case TokenKind::Continue: case TokenKind::Exit:
        case TokenKind::Yield: case TokenKind::Inline:
        case TokenKind::Integer: case TokenKind::Int:
        case TokenKind::Boolean: case TokenKind::Bool:
        case TokenKind::String: case TokenKind::Float: case TokenKind::Unsigned:
        case TokenKind::List: case TokenKind::Dict: case TokenKind::Nothing:
            return true;
        default:
            return false;
    }
}

bool Parser::isDeclStart() const {
    switch (cur_.kind) {
        case TokenKind::Class: case TokenKind::Namespace:
        case TokenKind::Include: case TokenKind::Import:
        case TokenKind::Start:
        case TokenKind::Public: case TokenKind::Private: case TokenKind::Protected:
        case TokenKind::Create: case TokenKind::Destroy:
        case TokenKind::Integer: case TokenKind::Int:
        case TokenKind::Boolean: case TokenKind::Bool:
        case TokenKind::String: case TokenKind::Float: case TokenKind::Unsigned:
        case TokenKind::List: case TokenKind::Dict: case TokenKind::Nothing:
            return true;
        default:
            return false;
    }
}

void Parser::synchronizeStmt() {
    while (!check(TokenKind::Eof) && !check(TokenKind::RBrace)) {
        if (isStmtStart()) { panic_ = false; return; }
        advance();
    }
    panic_ = false;
}

void Parser::synchronizeDecl() {
    while (!check(TokenKind::Eof) && !check(TokenKind::RBrace) && !check(TokenKind::Start)) {
        if (isDeclStart()) { panic_ = false; return; }
        advance();
    }
    panic_ = false;
}


// ---- Types & names ----

AstPtr Parser::parseCompoundName() {
    auto n = std::make_unique<AstNode>(AstKind::CompoundName, cur_.line, cur_.column);
    if (!check(TokenKind::Identifier)) {
        error("expected identifier");
        return n;
    }
    n->nameParts.push_back(cur_.lexeme);
    advance();
    while (match(TokenKind::Dot)) {
        if (!check(TokenKind::Identifier)) { error("expected identifier after '.'"); break; }
        n->nameParts.push_back(cur_.lexeme);
        advance();
    }
    return n;
}

AstPtr Parser::parseTypeSpecifier() {
    auto n = std::make_unique<AstNode>(AstKind::TypeSpec, cur_.line, cur_.column);
    switch (cur_.kind) {
        case TokenKind::Integer: case TokenKind::Int:
        case TokenKind::Boolean: case TokenKind::Bool:
        case TokenKind::String: case TokenKind::Dict: case TokenKind::List:
        case TokenKind::Unsigned: case TokenKind::Float: case TokenKind::Nothing:
        case TokenKind::Null:
            n->isLiteralType = true;
            n->typeTok = cur_.kind;
            advance();
            return n;
        default:
            n->isLiteralType = false;
            n->left = parseCompoundName();
            if (match(TokenKind::LtOp)) {
                if (!check(TokenKind::GtOp)) {
                    do { n->args.push_back(parseTypeSpecifier()); }
                    while (match(TokenKind::Comma));
                }
                consume(TokenKind::GtOp, "expected '>' after template arguments");
            }
            return n;
    }
}

// ---- Expressions (precedence climbing) ----

static int precedence(TokenKind k) {
    switch (k) {
        case TokenKind::OrOr: case TokenKind::Or: return 1;
        case TokenKind::AndAnd: case TokenKind::And: return 2;
        case TokenKind::Eq: case TokenKind::Ne: case TokenKind::Equ: case TokenKind::Neq: return 3;
        case TokenKind::LtOp: case TokenKind::GtOp: case TokenKind::Le: case TokenKind::Ge:
        case TokenKind::Lt: case TokenKind::Gt: case TokenKind::Lte: case TokenKind::Gte: return 4;
        case TokenKind::Plus: case TokenKind::Minus: return 5;
        case TokenKind::Star: case TokenKind::Slash: case TokenKind::Percent: return 6;
        case TokenKind::Caret: return 7;
        default: return -1;
    }
}

AstPtr Parser::parsePrimary() {
    if (check(TokenKind::Super)) {
        auto n = std::make_unique<AstNode>(AstKind::FunctionCall, cur_.line, cur_.column);
        n->text = "super";
        advance();
        if (!check(TokenKind::Identifier)) { error("expected base class after super"); return n; }
        n->className = cur_.lexeme;
        advance();
        consume(TokenKind::Dot, "expected '.' after super Base");
        if (!(check(TokenKind::Identifier) || check(TokenKind::Create) || check(TokenKind::Destroy))) {
            error("expected method name after super Base.");
            return n;
        }
        n->name = cur_.lexeme;
        advance();
        consume(TokenKind::LParen, "expected '(' after super call");
        if (!check(TokenKind::RParen)) {
            do { n->args.push_back(parseExpression()); }
            while (match(TokenKind::Comma));
        }
        consume(TokenKind::RParen, "expected ')' after super args");
        return n;
    }
    if (check(TokenKind::Copy)) {
        auto n = std::make_unique<AstNode>(AstKind::UnaryOp, cur_.line, cur_.column);
        n->op = TokenKind::Copy;
        advance();
        n->left = parsePrimary();
        return n;
    }
    if (check(TokenKind::Type)) {
        auto n = std::make_unique<AstNode>(AstKind::TypeOf, cur_.line, cur_.column);
        advance();
        consume(TokenKind::LParen, "expected '(' after type");
        n->left = parseExpression();
        consume(TokenKind::RParen, "expected ')' after type(...)");
        return n;
    }
    if (check(TokenKind::Bang) || check(TokenKind::Not) || check(TokenKind::Minus)) {
        auto n = std::make_unique<AstNode>(AstKind::UnaryOp, cur_.line, cur_.column);
        n->op = cur_.kind;
        advance();
        n->left = parsePrimary();
        return n;
    }
    if (match(TokenKind::LParen)) {
        bool typeKw = check(TokenKind::Integer) || check(TokenKind::Int) ||
            check(TokenKind::Boolean) || check(TokenKind::Bool) ||
            check(TokenKind::String) || check(TokenKind::Float) ||
            check(TokenKind::Unsigned) || check(TokenKind::List) ||
            check(TokenKind::Dict) || check(TokenKind::Nothing);
        if (typeKw) {
            auto c = std::make_unique<AstNode>(AstKind::TypeCast, cur_.line, cur_.column);
            c->type = parseTypeSpecifier();
            consume(TokenKind::RParen, "expected ')' after cast type");
            c->left = parsePrimary();
            return c;
        }
        if (check(TokenKind::Identifier)) {
            Token id = cur_;
            advance();
            if (match(TokenKind::RParen)) {
                bool nextPrimary = check(TokenKind::Identifier) || check(TokenKind::LitInt) ||
                    check(TokenKind::LitUns) || check(TokenKind::LitFloat) ||
                    check(TokenKind::LitBool) || check(TokenKind::LitSStr) ||
                    check(TokenKind::LitDStr) || check(TokenKind::LParen) ||
                    check(TokenKind::LBracket) || check(TokenKind::LBrace) ||
                    check(TokenKind::Minus) || check(TokenKind::Bang) || check(TokenKind::Not);
                if (nextPrimary) {
                    auto c = std::make_unique<AstNode>(AstKind::TypeCast, id.line, id.column);
                    auto ts = std::make_unique<AstNode>(AstKind::TypeSpec, id.line, id.column);
                    ts->text = id.lexeme;
                    ts->isLiteralType = false;
                    c->type = std::move(ts);
                    c->text = "safe";
                    c->left = parsePrimary();
                    return c;
                }
                auto ident = std::make_unique<AstNode>(AstKind::Identifier, id.line, id.column);
                ident->name = id.lexeme;
                return ident;
            }
            auto ident = std::make_unique<AstNode>(AstKind::Identifier, id.line, id.column);
            ident->name = id.lexeme;
            auto left = parseExpressionCont(std::move(ident), 0);
            consume(TokenKind::RParen, "expected ')'");
            return left;
        }
        auto e = parseExpression();
        consume(TokenKind::RParen, "expected ')'");
        return e;
    }
    if (check(TokenKind::Null)) {
        auto n = std::make_unique<AstNode>(AstKind::Literal, cur_.line, cur_.column);
        n->litKind = TokenKind::Null;
        n->text = "null";
        advance();
        return n;
    }
    if (check(TokenKind::LitInt) || check(TokenKind::LitUns) || check(TokenKind::LitFloat) ||
        check(TokenKind::LitBool) || check(TokenKind::LitSStr) || check(TokenKind::LitDStr)) {
        auto n = std::make_unique<AstNode>(AstKind::Literal, cur_.line, cur_.column);
        n->litKind = cur_.kind;
        n->text = cur_.lexeme;
        advance();
        // Formatted string: "pattern {} {name}"(expr, name=expr)
        if ((n->litKind == TokenKind::LitSStr || n->litKind == TokenKind::LitDStr) &&
            match(TokenKind::LParen)) {
            auto fmt = std::make_unique<AstNode>(AstKind::FormatString, n->line, n->column);
            fmt->text = n->text;
            if (!check(TokenKind::RParen)) {
                do {
                    auto arg = std::make_unique<AstNode>(AstKind::FormatArg, cur_.line, cur_.column);
                    // name = expr  OR  expr
                    // Named arg: ident '=' expr
                    // Positional: full expression (including a+b, calls, etc.)
                    if (check(TokenKind::Identifier)) {
                        Token id = cur_;
                        // Lookahead: only treat as name= if next token is '='
                        // We advance id, then check Assign
                        advance();
                        if (match(TokenKind::Assign)) {
                            arg->name = id.lexeme;
                            arg->left = parseExpression();
                        } else {
                            // Rebuild primary from identifier (+ optional call / .member)
                            AstPtr primary;
                            if (check(TokenKind::LParen) || check(TokenKind::Dot)) {
                                // compound ref starting with this id
                                auto ref = std::make_unique<AstNode>(AstKind::CompoundRef, id.line, id.column);
                                if (match(TokenKind::LParen)) {
                                    auto call = std::make_unique<AstNode>(AstKind::FunctionCall, id.line, id.column);
                                    call->name = id.lexeme;
                                    if (!check(TokenKind::RParen)) {
                                        do { call->args.push_back(parseExpression()); }
                                        while (match(TokenKind::Comma));
                                    }
                                    consume(TokenKind::RParen, "expected ')'");
                                    ref->children.push_back(std::move(call));
                                } else {
                                    auto idn = std::make_unique<AstNode>(AstKind::Identifier, id.line, id.column);
                                    idn->name = id.lexeme;
                                    ref->children.push_back(std::move(idn));
                                }
                                while (match(TokenKind::Dot)) {
                                    if (!(check(TokenKind::Identifier) || check(TokenKind::Create) || check(TokenKind::Destroy))) {
                                        error("expected member name");
                                        break;
                                    }
                                    auto m = std::make_unique<AstNode>(AstKind::Identifier, cur_.line, cur_.column);
                                    m->name = cur_.lexeme;
                                    advance();
                                    if (match(TokenKind::LParen)) {
                                        auto call = std::make_unique<AstNode>(AstKind::FunctionCall, m->line, m->column);
                                        call->name = m->name;
                                        if (!check(TokenKind::RParen)) {
                                            do { call->args.push_back(parseExpression()); }
                                            while (match(TokenKind::Comma));
                                        }
                                        consume(TokenKind::RParen, "expected ')'");
                                        ref->children.push_back(std::move(call));
                                    } else {
                                        ref->children.push_back(std::move(m));
                                    }
                                }
                                primary = std::move(ref);
                            } else {
                                auto idn = std::make_unique<AstNode>(AstKind::Identifier, id.line, id.column);
                                idn->name = id.lexeme;
                                primary = std::move(idn);
                            }
                            // Continue with binary operators: a + b * c ...
                            arg->left = parseExpressionCont(std::move(primary), 0);
                        }
                    } else {
                        arg->left = parseExpression();
                    }
                    fmt->children.push_back(std::move(arg));
                } while (match(TokenKind::Comma));
            }
            consume(TokenKind::RParen, "expected ')' after format args");
            return fmt;
        }
        return n;
    }
    if (check(TokenKind::Identifier) || check(TokenKind::Create) || check(TokenKind::Destroy)) {
        // compound_reference: item (. item)*
        auto ref = std::make_unique<AstNode>(AstKind::CompoundRef, cur_.line, cur_.column);
        auto item = std::make_unique<AstNode>(AstKind::Identifier, cur_.line, cur_.column);
        item->name = cur_.lexeme;
        advance();
        if (match(TokenKind::LtOp)) {
            if (!check(TokenKind::GtOp)) {
                do { item->args.push_back(parseTypeSpecifier()); }
                while (match(TokenKind::Comma));
            }
            consume(TokenKind::GtOp, "expected '>' after template arguments");
        }
        // call?
        if (match(TokenKind::LParen)) {
            auto call = std::make_unique<AstNode>(AstKind::FunctionCall, item->line, item->column);
            call->name = item->name;
            for (auto& a : item->args) call->children.push_back(std::move(a));
            if (!check(TokenKind::RParen)) {
                do {
                    call->args.push_back(parseExpression());
                } while (match(TokenKind::Comma));
            }
            consume(TokenKind::RParen, "expected ')'");
            ref->children.push_back(std::move(call));
        } else {
            ref->children.push_back(std::move(item));
        }
        while (match(TokenKind::Dot)) {
            if (!(check(TokenKind::Identifier) || check(TokenKind::Create) || check(TokenKind::Destroy))) {
                error("expected member name");
                break;
            }
            auto m = std::make_unique<AstNode>(AstKind::Identifier, cur_.line, cur_.column);
            m->name = cur_.lexeme;
            advance();
            if (match(TokenKind::LParen)) {
                auto call = std::make_unique<AstNode>(AstKind::FunctionCall, m->line, m->column);
                call->name = m->name;
                if (!check(TokenKind::RParen)) {
                    do { call->args.push_back(parseExpression()); } while (match(TokenKind::Comma));
                }
                consume(TokenKind::RParen, "expected ')'");
                ref->children.push_back(std::move(call));
            } else {
                ref->children.push_back(std::move(m));
            }
        }
        return ref;
    }
    if (check(TokenKind::LBracket)) {
        auto n = std::make_unique<AstNode>(AstKind::ArrayLiteral, cur_.line, cur_.column);
        advance();
        if (!check(TokenKind::RBracket)) {
            do { n->children.push_back(parseExpression()); }
            while (match(TokenKind::Comma));
        }
        consume(TokenKind::RBracket, "expected ']' after list");
        return n;
    }
    if (check(TokenKind::LBrace)) {
        auto n = std::make_unique<AstNode>(AstKind::DictLiteral, cur_.line, cur_.column);
        advance();
        if (!check(TokenKind::RBrace)) {
            do {
                auto item = std::make_unique<AstNode>(AstKind::DictItem, cur_.line, cur_.column);
                if (check(TokenKind::Identifier)) {
                    item->name = cur_.lexeme;
                    advance();
                } else {
                    item->left = parseExpression();
                }
                consume(TokenKind::Colon, "expected ':' in dict item");
                item->right = parseExpression();
                n->children.push_back(std::move(item));
            } while (match(TokenKind::Comma));
        }
        consume(TokenKind::RBrace, "expected '}' after dict");
        return n;
    }
    error("expected expression");
    if (!isStmtStart() && !isDeclStart() && !check(TokenKind::RBrace) && !check(TokenKind::Eof))
        advance();
    return std::make_unique<AstNode>(AstKind::Literal, cur_.line, cur_.column);
}

AstPtr Parser::parseExpressionPrec(int minPrec) {
    auto left = parsePrimary();
    if (match(TokenKind::Is)) {
        auto n = std::make_unique<AstNode>(AstKind::BinaryOp, cur_.line, cur_.column);
        n->op = TokenKind::Is;
        n->left = std::move(left);
        n->right = parseCompoundName();
        left = std::move(n);
    }
    if (match(TokenKind::As)) {
        auto c = std::make_unique<AstNode>(AstKind::TypeCast, cur_.line, cur_.column);
        c->text = "safe";
        auto ts = std::make_unique<AstNode>(AstKind::TypeSpec, cur_.line, cur_.column);
        if (check(TokenKind::Identifier)) {
            ts->text = cur_.lexeme;
            ts->isLiteralType = false;
            auto cn = std::make_unique<AstNode>(AstKind::CompoundName, cur_.line, cur_.column);
            cn->nameParts.push_back(cur_.lexeme);
            ts->left = std::move(cn);
            advance();
        } else {
            ts = parseTypeSpecifier();
        }
        c->type = std::move(ts);
        c->left = std::move(left);
        left = std::move(c);
    }
    while (check(TokenKind::LBracket)) {
        auto ref = std::make_unique<AstNode>(AstKind::ArrayRef, cur_.line, cur_.column);
        advance();
        ref->left = std::move(left);
        ref->right = parseExpression();
        consume(TokenKind::RBracket, "expected ']'");
        left = std::move(ref);
    }
    for (;;) {
        int prec = precedence(cur_.kind);
        if (prec < minPrec) break;
        auto n = std::make_unique<AstNode>(AstKind::BinaryOp, cur_.line, cur_.column);
        n->op = cur_.kind;
        advance();
        int nextMin = (n->op == TokenKind::Caret) ? prec : prec + 1;
        n->left = std::move(left);
        n->right = parseExpressionPrec(nextMin);
        left = std::move(n);
    }
    return left;
}

AstPtr Parser::parseExpressionCont(AstPtr left, int minPrec) {
    while (check(TokenKind::LBracket)) {
        auto ref = std::make_unique<AstNode>(AstKind::ArrayRef, cur_.line, cur_.column);
        advance();
        ref->left = std::move(left);
        ref->right = parseExpression();
        consume(TokenKind::RBracket, "expected ']'");
        left = std::move(ref);
    }
    for (;;) {
        int prec = precedence(cur_.kind);
        if (prec < minPrec) break;
        auto n = std::make_unique<AstNode>(AstKind::BinaryOp, cur_.line, cur_.column);
        n->op = cur_.kind;
        advance();
        int nextMin = (n->op == TokenKind::Caret) ? prec : prec + 1;
        n->left = std::move(left);
        n->right = parseExpressionPrec(nextMin);
        left = std::move(n);
    }
    return left;
}

AstPtr Parser::parseExpression() {
    return parseExpressionPrec(0);
}

// ---- Bodies ----

AstPtr Parser::parseBodyItem() {
    if (check(TokenKind::If)) {
        auto n = std::make_unique<AstNode>(AstKind::If, cur_.line, cur_.column);
        advance();
        consume(TokenKind::LParen, "expected '('");
        n->condition = parseExpression();
        consume(TokenKind::RParen, "expected ')'");
        n->body = parseFunctionBody();
        /* else_clause* final_else_clause? */
        AstNode* tail = n.get();
        while (check(TokenKind::Else)) {
            advance();
            /* optional "if" keyword: else if (cond) */
            match(TokenKind::If);
            if (match(TokenKind::LParen)) {
                if (match(TokenKind::RParen)) {
                    /* final else () { } */
                    tail->elseBody = parseFunctionBody();
                    break;
                }
                auto elif = std::make_unique<AstNode>(AstKind::If, cur_.line, cur_.column);
                elif->condition = parseExpression();
                consume(TokenKind::RParen, "expected ')' after else (cond)");
                elif->body = parseFunctionBody();
                AstNode* next = elif.get();
                tail->elseBody = std::move(elif);
                tail = next;
                continue;
            }
            /* final else { } */
            tail->elseBody = parseFunctionBody();
            break;
        }
        return n;
    }
    if (check(TokenKind::While)) {
        auto n = std::make_unique<AstNode>(AstKind::While, cur_.line, cur_.column);
        advance();
        if (match(TokenKind::LParen)) {
            if (!check(TokenKind::RParen)) n->condition = parseExpression();
            consume(TokenKind::RParen, "expected ')'");
        }
        n->body = parseLoopBody();
        return n;
    }
    if (check(TokenKind::For)) {
        auto n = std::make_unique<AstNode>(AstKind::For, cur_.line, cur_.column);
        advance();
        if (match(TokenKind::LParen)) {
            if (!check(TokenKind::RParen)) {
                n->condition = parsePrimary();
                if (n->condition)
                    n->condition = parseExpressionCont(std::move(n->condition), 0);
                if (match(TokenKind::As)) {
                    if (check(TokenKind::Integer) || check(TokenKind::Int) ||
                        check(TokenKind::Unsigned) || check(TokenKind::Float) ||
                        check(TokenKind::Boolean) || check(TokenKind::Bool) ||
                        check(TokenKind::String) || check(TokenKind::Nothing) ||
                        check(TokenKind::List) || check(TokenKind::Dict) ||
                        check(TokenKind::Identifier)) {
                        // optional type then name, or just name
                        bool looksType = check(TokenKind::Integer) || check(TokenKind::Int) ||
                            check(TokenKind::Unsigned) || check(TokenKind::Float) ||
                            check(TokenKind::Boolean) || check(TokenKind::Bool) ||
                            check(TokenKind::String) || check(TokenKind::Nothing) ||
                            check(TokenKind::List) || check(TokenKind::Dict);
                        if (looksType)
                            n->type = parseTypeSpecifier();
                        if (check(TokenKind::Identifier)) {
                            n->name = cur_.lexeme;
                            advance();
                        }
                    }
                }
            }
            consume(TokenKind::RParen, "expected ')' after for");
        }
        n->body = parseLoopBody();
        return n;
    }
    if (check(TokenKind::Do)) {
        auto n = std::make_unique<AstNode>(AstKind::DoWhile, cur_.line, cur_.column);
        advance();
        n->body = parseLoopBody();
        consume(TokenKind::While, "expected 'while' after do body");
        if (match(TokenKind::LParen)) {
            if (!check(TokenKind::RParen)) n->condition = parseExpression();
            consume(TokenKind::RParen, "expected ')' after do-while");
        }
        return n;
    }
    if (check(TokenKind::Exit)) {
        auto n = std::make_unique<AstNode>(AstKind::Exit, cur_.line, cur_.column);
        advance();
        consume(TokenKind::LParen, "expected '(' after exit");
        if (!check(TokenKind::RParen)) n->left = parseExpression();
        consume(TokenKind::RParen, "expected ')' after exit");
        return n;
    }
    if (check(TokenKind::Return)) {
        auto n = std::make_unique<AstNode>(AstKind::Return, cur_.line, cur_.column);
        advance();
        if (match(TokenKind::LParen)) {
            if (!check(TokenKind::RParen)) n->left = parseExpression();
            consume(TokenKind::RParen, "expected ')'");
        }
        return n;
    }
    if (check(TokenKind::Raise)) {
        auto n = std::make_unique<AstNode>(AstKind::Raise, cur_.line, cur_.column);
        advance();
        consume(TokenKind::LParen, "expected '(' after raise");
        n->left = parseCompoundName();  /* exception type name */
        consume(TokenKind::RParen, "expected ')'");
        return n;
    }

    if (check(TokenKind::Print)) {
        auto n = std::make_unique<AstNode>(AstKind::Print, cur_.line, cur_.column);
        advance();
        // print expr   or   print ( expr )
        if (match(TokenKind::LParen)) {
            n->left = parseExpression();
            consume(TokenKind::RParen, "expected ')' after print");
        } else {
            n->left = parseExpression();
        }
        return n;
    }
    if (check(TokenKind::Try)) {
        auto n = std::make_unique<AstNode>(AstKind::Try, cur_.line, cur_.column);
        advance();
        n->body = parseFunctionBody();
        /* one or more except clauses */
        while (check(TokenKind::Except)) {
            auto ex = std::make_unique<AstNode>(AstKind::Except, cur_.line, cur_.column);
            advance();
            consume(TokenKind::LParen, "expected '(' after except");
            ex->left = parseCompoundName();  /* exception type */
            if (match(TokenKind::As) && check(TokenKind::Identifier)) {
                ex->name = cur_.lexeme;
                advance();
            }
            consume(TokenKind::RParen, "expected ')'");
            ex->body = parseFunctionBody();
            n->children.push_back(std::move(ex));
        }
        if (check(TokenKind::Finally)) {
            advance();
            n->elseBody = parseFunctionBody();  /* reuse elseBody for finally */
        }
        return n;
    }
    if (check(TokenKind::Break)) { advance(); return std::make_unique<AstNode>(AstKind::Break, cur_.line, cur_.column); }
    if (check(TokenKind::Continue)) { advance(); return std::make_unique<AstNode>(AstKind::Continue, cur_.line, cur_.column); }
    if (check(TokenKind::Inline)) {
        auto n = std::make_unique<AstNode>(AstKind::Inline, cur_.line, cur_.column);
        advance();
        if (match(TokenKind::LParen)) {
            if (!check(TokenKind::RParen)) {
                do {
                    if (!check(TokenKind::Identifier)) { error("expected name in inline (...)"); break; }
                    n->nameParts.push_back(cur_.lexeme);
                    advance();
                } while (match(TokenKind::Comma));
            }
            consume(TokenKind::RParen, "expected ')' after inline imports");
        }
        if (!check(TokenKind::LBrace)) {
            error("expected '{' after inline");
            return n;
        }
        /* cur_ is '{'; lexer pos is already past it. Do not tokenize the C. */
        n->text = lex_.readUntilMatchingRBrace();
        advance();
        return n;
    }
    if (check(TokenKind::Yield)) {
        auto n = std::make_unique<AstNode>(AstKind::Yield, cur_.line, cur_.column);
        advance();
        consume(TokenKind::LParen, "expected '(' after yield");
        if (!check(TokenKind::RParen)) n->left = parseExpression();
        consume(TokenKind::RParen, "expected ')' after yield");
        return n;
    }

    // data definition or assignment / expression statement
    // Heuristic: type-like token then identifier → data
    if (check(TokenKind::Integer) || check(TokenKind::Int) || check(TokenKind::Boolean) ||
        check(TokenKind::Bool) || check(TokenKind::String) || check(TokenKind::Float) ||
        check(TokenKind::Unsigned) || check(TokenKind::List) || check(TokenKind::Dict) ||
        check(TokenKind::Nothing)) {
        auto d = std::make_unique<AstNode>(AstKind::DataDecl, cur_.line, cur_.column);
        d->type = parseTypeSpecifier();
        if (!check(TokenKind::Identifier)) { error("expected name"); return d; }
        d->name = cur_.lexeme;
        advance();
        if (match(TokenKind::Assign)) d->init = parseExpression();
        return d;
    }

    /* ClassName var  /  ClassName var = expr */
    if (check(TokenKind::Identifier)) {
        Token t0 = cur_;
        advance();
        std::vector<AstPtr> targs;
        if (match(TokenKind::LtOp)) {
            if (!check(TokenKind::GtOp)) {
                do { targs.push_back(parseTypeSpecifier()); }
                while (match(TokenKind::Comma));
            }
            consume(TokenKind::GtOp, "expected '>' after template arguments");
        }
        if (check(TokenKind::Identifier) && !targs.empty()) {
            auto d = std::make_unique<AstNode>(AstKind::DataDecl, t0.line, t0.column);
            auto ts = std::make_unique<AstNode>(AstKind::TypeSpec, t0.line, t0.column);
            ts->isLiteralType = false;
            auto cn = std::make_unique<AstNode>(AstKind::CompoundName, t0.line, t0.column);
            cn->nameParts.push_back(t0.lexeme);
            ts->left = std::move(cn);
            ts->text = t0.lexeme;
            ts->args = std::move(targs);
            d->type = std::move(ts);
            d->name = cur_.lexeme;
            advance();
            if (match(TokenKind::Assign)) d->init = parseExpression();
            return d;
        }
        if (check(TokenKind::Identifier)) {
            auto d = std::make_unique<AstNode>(AstKind::DataDecl, t0.line, t0.column);
            auto ts = std::make_unique<AstNode>(AstKind::TypeSpec, t0.line, t0.column);
            ts->isLiteralType = false;
            auto cn = std::make_unique<AstNode>(AstKind::CompoundName, t0.line, t0.column);
            cn->nameParts.push_back(t0.lexeme);
            ts->left = std::move(cn);
            ts->text = t0.lexeme;
            d->type = std::move(ts);
            d->name = cur_.lexeme;
            advance();
            if (match(TokenKind::Assign)) d->init = parseExpression();
            return d;
        }
        /* rebuild primary from t0 and continue as expression / assignment */
        auto idn = std::make_unique<AstNode>(AstKind::Identifier, t0.line, t0.column);
        idn->name = t0.lexeme;
        auto refn = std::make_unique<AstNode>(AstKind::CompoundRef, t0.line, t0.column);
        if (match(TokenKind::LParen)) {
            auto call = std::make_unique<AstNode>(AstKind::FunctionCall, t0.line, t0.column);
            call->name = t0.lexeme;
            for (auto& a : targs) call->children.push_back(std::move(a));
            if (!check(TokenKind::RParen)) {
                do { call->args.push_back(parseExpression()); }
                while (match(TokenKind::Comma));
            }
            consume(TokenKind::RParen, "expected ')'");
            refn->children.push_back(std::move(call));
        } else {
            for (auto& a : targs) idn->args.push_back(std::move(a));
            refn->children.push_back(std::move(idn));
        }
        while (match(TokenKind::Dot)) {
            if (!(check(TokenKind::Identifier) || check(TokenKind::Create) || check(TokenKind::Destroy))) {
                error("expected member name");
                break;
            }
            auto m = std::make_unique<AstNode>(AstKind::Identifier, cur_.line, cur_.column);
            m->name = cur_.lexeme;
            advance();
            if (match(TokenKind::LParen)) {
                auto call = std::make_unique<AstNode>(AstKind::FunctionCall, m->line, m->column);
                call->name = m->name;
                if (!check(TokenKind::RParen)) {
                    do { call->args.push_back(parseExpression()); }
                    while (match(TokenKind::Comma));
                }
                consume(TokenKind::RParen, "expected ')'");
                refn->children.push_back(std::move(call));
            } else {
                refn->children.push_back(std::move(m));
            }
        }
        auto left = parseExpressionCont(std::move(refn), 0);
        if (match(TokenKind::Assign)) {
            auto a = std::make_unique<AstNode>(AstKind::Assignment, t0.line, t0.column);
            a->left = std::move(left);
            a->right = parseExpression();
            return a;
        }
        return left;
    }

    auto ref = parseExpression();
    if (match(TokenKind::Assign)) {
        auto a = std::make_unique<AstNode>(AstKind::Assignment, ref->line, ref->column);
        a->left = std::move(ref);
        a->right = parseExpression();
        return a;
    }
    return ref;
}

AstPtr Parser::parseFunctionBody() {
    auto body = std::make_unique<AstNode>(AstKind::Body, cur_.line, cur_.column);
    consume(TokenKind::LBrace, "expected '{'");
    while (!check(TokenKind::RBrace) && !check(TokenKind::Eof)) {
        auto stmt = parseBodyItem();
        if (stmt) body->children.push_back(std::move(stmt));
        if (panic_) synchronizeStmt();
        else panic_ = false;
    }
    consume(TokenKind::RBrace, "expected '}'");
    return body;
}

AstPtr Parser::parseLoopBody() {
    return parseFunctionBody(); // same structure for simplicity
}

// ---- Class / namespace / module ----

AstPtr Parser::parseConstructor(bool) {
    auto n = std::make_unique<AstNode>(AstKind::Constructor, cur_.line, cur_.column);
    consume(TokenKind::Create, "expected 'create'");
    n->name = "create";
    consume(TokenKind::LParen, "expected '('");
    if (!check(TokenKind::RParen)) {
        do {
            auto p = std::make_unique<AstNode>(AstKind::Param, cur_.line, cur_.column);
            p->type = parseTypeSpecifier();
            if (check(TokenKind::Identifier)) { p->name = cur_.lexeme; advance(); }
            n->params.push_back(std::move(p));
        } while (match(TokenKind::Comma));
    }
    consume(TokenKind::RParen, "expected ')'");
    if (check(TokenKind::LBrace)) n->body = parseFunctionBody();
    return n;
}

AstPtr Parser::parseDestructor(bool) {
    auto n = std::make_unique<AstNode>(AstKind::Destructor, cur_.line, cur_.column);
    consume(TokenKind::Destroy, "expected 'destroy'");
    n->name = "destroy";
    if (check(TokenKind::LBrace)) n->body = parseFunctionBody();
    return n;
}

AstPtr Parser::parseClass() {
    auto cls = std::make_unique<AstNode>(AstKind::Class, cur_.line, cur_.column);
    consume(TokenKind::Class, "expected 'class'");
    if (!check(TokenKind::Identifier)) { error("expected class name"); return cls; }
    cls->name = cur_.lexeme;
    advance();
    if (match(TokenKind::LtOp)) {
        if (!check(TokenKind::GtOp)) {
            do {
                if (!check(TokenKind::Identifier)) { error("expected template parameter"); break; }
                cls->nameParts.push_back(cur_.lexeme);
                advance();
            } while (match(TokenKind::Comma));
        }
        consume(TokenKind::GtOp, "expected '>' after template parameters");
    }
    // optional inheritance list: class Foo (public Base, ...)
    if (match(TokenKind::LParen)) {
        while (!check(TokenKind::RParen) && !check(TokenKind::Eof)) {
            if (check(TokenKind::Public) || check(TokenKind::Private) || check(TokenKind::Protected))
                advance();
            auto cn = parseCompoundName();
            if (cn && !cn->nameParts.empty())
                cls->bases.push_back(cn->nameParts.back());
            if (!match(TokenKind::Comma)) break;
        }
        consume(TokenKind::RParen, "expected ')'");
    }
    consume(TokenKind::LBrace, "expected '{'");
    Visibility vis = Visibility::Private;
    while (!check(TokenKind::RBrace) && !check(TokenKind::Eof)) {
        if (match(TokenKind::Public))    { vis = Visibility::Public; continue; }
        if (match(TokenKind::Private))   { vis = Visibility::Private; continue; }
        if (match(TokenKind::Protected)) { vis = Visibility::Protected; continue; }

        bool isVirt = match(TokenKind::Virtual);
        if (check(TokenKind::Create)) {
            auto m = parseConstructor(false);
            m->visibility = vis;
            m->isVirtual = isVirt;
            cls->children.push_back(std::move(m));
        } else if (check(TokenKind::Destroy)) {
            auto m = parseDestructor(false);
            m->visibility = vis;
            m->isVirtual = isVirt;
            cls->children.push_back(std::move(m));
        } else {
            auto ty = parseTypeSpecifier();
            if (!check(TokenKind::Identifier)) { error("expected member name"); break; }
            std::string name = cur_.lexeme;
            int line = cur_.line, col = cur_.column;
            advance();
            if (check(TokenKind::LParen)) {
                auto fn = std::make_unique<AstNode>(AstKind::Function, line, col);
                fn->visibility = vis;
                fn->isVirtual = isVirt;
                fn->type = std::move(ty);
                fn->name = name;
                if (match(TokenKind::LtOp)) {
                    if (!check(TokenKind::GtOp)) {
                        do {
                            if (!check(TokenKind::Identifier)) { error("expected template parameter"); break; }
                            fn->nameParts.push_back(cur_.lexeme);
                            advance();
                        } while (match(TokenKind::Comma));
                    }
                    consume(TokenKind::GtOp, "expected '>' after template parameters");
                }
                advance(); // (
                if (!check(TokenKind::RParen)) {
                    do {
                        auto p = std::make_unique<AstNode>(AstKind::Param, cur_.line, cur_.column);
                        p->type = parseTypeSpecifier();
                        if (check(TokenKind::Identifier)) { p->name = cur_.lexeme; advance(); }
                        fn->params.push_back(std::move(p));
                    } while (match(TokenKind::Comma));
                }
                consume(TokenKind::RParen, "expected ')'");
                if (check(TokenKind::LBrace)) fn->body = parseFunctionBody();
                cls->children.push_back(std::move(fn));
            } else {
                auto d = std::make_unique<AstNode>(AstKind::DataDecl, line, col);
                d->visibility = vis;
                d->type = std::move(ty);
                d->name = name;
                if (match(TokenKind::Assign)) d->init = parseExpression();
                cls->children.push_back(std::move(d));
            }
        }
        if (panic_) synchronizeDecl();
        else panic_ = false;
    }
    consume(TokenKind::RBrace, "expected '}'");
    return cls;
}

AstPtr Parser::parseNamespace() {
    auto ns = std::make_unique<AstNode>(AstKind::Namespace, cur_.line, cur_.column);
    consume(TokenKind::Namespace, "expected 'namespace'");
    if (!check(TokenKind::Identifier)) { error("expected namespace name"); return ns; }
    ns->name = cur_.lexeme;
    advance();
    consume(TokenKind::LBrace, "expected '{'");
    Visibility vis = Visibility::Public;
    while (!check(TokenKind::RBrace) && !check(TokenKind::Eof)) {
        if (match(TokenKind::Public))  { vis = Visibility::Public; continue; }
        if (match(TokenKind::Private)) { vis = Visibility::Private; continue; }
        if (match(TokenKind::Protected)) { vis = Visibility::Protected; continue; }
        if (check(TokenKind::Class)) {
            auto c = parseClass();
            c->visibility = vis;
            ns->children.push_back(std::move(c));
        } else if (check(TokenKind::Namespace)) {
            ns->children.push_back(parseNamespace());
        } else if (check(TokenKind::Create) ||
                   check(TokenKind::Integer) || check(TokenKind::Int) ||
                   check(TokenKind::Boolean) || check(TokenKind::Bool) ||
                   check(TokenKind::String) || check(TokenKind::Dict) || check(TokenKind::List) ||
                   check(TokenKind::Unsigned) || check(TokenKind::Float) || check(TokenKind::Nothing) ||
                   check(TokenKind::Identifier)) {
            auto def = parseOutOfClassDef();
            if (def) ns->children.push_back(std::move(def));
        } else {
            error("unexpected item in namespace");
            advance();
        }
        if (panic_) synchronizeDecl();
        else panic_ = false;
    }
    consume(TokenKind::RBrace, "expected '}'");
    return ns;
}


AstPtr Parser::parseOutOfClassDef() {
    /* type Class.method (params) { body }
       create Class (params) { body }
    */
    if (check(TokenKind::Create)) {
        auto n = std::make_unique<AstNode>(AstKind::Constructor, cur_.line, cur_.column);
        advance();
        if (!check(TokenKind::Identifier)) { error("expected class name after create"); return n; }
        n->className = cur_.lexeme;
        n->name = "create";
        advance();
        consume(TokenKind::LParen, "expected '('");
        if (!check(TokenKind::RParen)) {
            do {
                auto p = std::make_unique<AstNode>(AstKind::Param, cur_.line, cur_.column);
                p->type = parseTypeSpecifier();
                if (check(TokenKind::Identifier)) { p->name = cur_.lexeme; advance(); }
                n->params.push_back(std::move(p));
            } while (match(TokenKind::Comma));
        }
        consume(TokenKind::RParen, "expected ')'");
        n->body = parseFunctionBody();
        return n;
    }
    if (check(TokenKind::Destroy)) {
        auto n = std::make_unique<AstNode>(AstKind::Destructor, cur_.line, cur_.column);
        advance();
        if (!check(TokenKind::Identifier)) { error("expected class name after destroy"); return n; }
        n->className = cur_.lexeme;
        n->name = "destroy";
        advance();
        n->body = parseFunctionBody();
        return n;
    }
    auto ty = parseTypeSpecifier();
    if (!check(TokenKind::Identifier)) { error("expected Class.method"); return nullptr; }
    std::string first = cur_.lexeme;
    int line = cur_.line, col = cur_.column;
    advance();
    std::string method;
    std::string cls = first;
    if (match(TokenKind::Dot)) {
        if (!check(TokenKind::Identifier) && !check(TokenKind::Create)) {
            error("expected method name after '.'");
            return nullptr;
        }
        method = cur_.lexeme;
        advance();
    } else {
        method = first;
        cls.clear();
    }
    auto fn = std::make_unique<AstNode>(AstKind::Function, line, col);
    fn->type = std::move(ty);
    fn->name = method;
    fn->className = cls;
    if (match(TokenKind::LtOp)) {
        if (!check(TokenKind::GtOp)) {
            do {
                if (!check(TokenKind::Identifier)) { error("expected template parameter"); break; }
                fn->nameParts.push_back(cur_.lexeme);
                advance();
            } while (match(TokenKind::Comma));
        }
        consume(TokenKind::GtOp, "expected '>' after template parameters");
    }
    consume(TokenKind::LParen, "expected '('");
    if (!check(TokenKind::RParen)) {
        do {
            auto p = std::make_unique<AstNode>(AstKind::Param, cur_.line, cur_.column);
            p->type = parseTypeSpecifier();
            if (check(TokenKind::Identifier)) { p->name = cur_.lexeme; advance(); }
            fn->params.push_back(std::move(p));
        } while (match(TokenKind::Comma));
    }
    consume(TokenKind::RParen, "expected ')'");
    fn->body = parseFunctionBody();
    return fn;
}

AstPtr Parser::parseModuleItem() {
    if (check(TokenKind::Include)) {
        advance();
        auto n = std::make_unique<AstNode>(AstKind::Include, cur_.line, cur_.column);
        if (check(TokenKind::LitSStr) || check(TokenKind::LitDStr)) {
            n->text = cur_.lexeme;
            advance();
        } else error("expected string after include");
        return n;
    }
    if (check(TokenKind::Import)) {
        advance();
        auto n = std::make_unique<AstNode>(AstKind::Import, cur_.line, cur_.column);
        if (check(TokenKind::LitSStr) || check(TokenKind::LitDStr)) {
            n->text = cur_.lexeme;
            advance();
        } else if (check(TokenKind::Identifier)) {
            n->text = cur_.lexeme;
            n->nameParts.push_back(cur_.lexeme);
            advance();
            while (match(TokenKind::Dot)) {
                if (!check(TokenKind::Identifier)) { error("expected identifier after '.'"); break; }
                n->text += ".";
                n->text += cur_.lexeme;
                n->nameParts.push_back(cur_.lexeme);
                advance();
            }
        } else {
            error("expected module path or package name after import");
        }
        if (match(TokenKind::As) && check(TokenKind::Identifier)) {
            n->name = cur_.lexeme;
            advance();
        }
        return n;
    }
    if (check(TokenKind::Namespace)) return parseNamespace();
    if (check(TokenKind::Class)) return parseClass();
    if (match(TokenKind::Public) || match(TokenKind::Private) || match(TokenKind::Protected))
        return parseModuleItem();
    if (check(TokenKind::Create) ||
        check(TokenKind::Integer) || check(TokenKind::Int) ||
        check(TokenKind::Boolean) || check(TokenKind::Bool) ||
        check(TokenKind::String) || check(TokenKind::Dict) || check(TokenKind::List) ||
        check(TokenKind::Unsigned) || check(TokenKind::Float) || check(TokenKind::Nothing) ||
        check(TokenKind::Identifier))
        return parseOutOfClassDef();
    error("unexpected module item");
    advance();
    return nullptr;
}

AstPtr Parser::parseModule() {
    auto mod = std::make_unique<AstNode>(AstKind::Module, 1, 1);
    while (!check(TokenKind::Eof) && !check(TokenKind::Start)) {
        auto item = parseModuleItem();
        if (item) mod->children.push_back(std::move(item));
        if (panic_) synchronizeDecl();
        else panic_ = false;
    }
    if (match(TokenKind::Start)) {
        auto st = std::make_unique<AstNode>(AstKind::Start, cur_.line, cur_.column);
        st->body = parseFunctionBody();
        mod->start = std::move(st);
    }
    return mod;
}
