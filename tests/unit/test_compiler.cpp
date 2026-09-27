/* Unit tests for lexer, parser, and semantics (no LLVM). */
#include "Lexer.hpp"
#include "Parser.hpp"
#include "Semant.hpp"
#include <cstdio>
#include <string>
#include <vector>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))

static std::vector<TokenKind> kinds(const char* src) {
    Lexer lex(src);
    std::vector<TokenKind> out;
    for (;;) {
        Token t = lex.next();
        out.push_back(t.kind);
        if (t.kind == TokenKind::Eof) break;
    }
    return out;
}

static void test_lexer() {
    auto k = kinds("start { print 1 }");
    CHECK(k.size() >= 5);
    CHECK_EQ(k[0], TokenKind::Start);
    CHECK_EQ(k[1], TokenKind::LBrace);
    CHECK_EQ(k[2], TokenKind::Print);
    CHECK_EQ(k[3], TokenKind::LitInt);
    CHECK_EQ(k[4], TokenKind::RBrace);

    k = kinds("class Box <T> { }");
    CHECK_EQ(k[0], TokenKind::Class);
    CHECK_EQ(k[1], TokenKind::Identifier);
    CHECK_EQ(k[2], TokenKind::LtOp);
    CHECK_EQ(k[3], TokenKind::Identifier);
    CHECK_EQ(k[4], TokenKind::GtOp);

    k = kinds("import greet as G");
    CHECK_EQ(k[0], TokenKind::Import);
    CHECK_EQ(k[1], TokenKind::Identifier);
    CHECK_EQ(k[2], TokenKind::As);
    CHECK_EQ(k[3], TokenKind::Identifier);

    k = kinds("# comment\nint x");
    CHECK_EQ(k[0], TokenKind::Int);
    CHECK_EQ(k[1], TokenKind::Identifier);

    k = kinds("create destroy virtual super copy is");
    CHECK_EQ(k[0], TokenKind::Create);
    CHECK_EQ(k[1], TokenKind::Destroy);
    CHECK_EQ(k[2], TokenKind::Virtual);
    CHECK_EQ(k[3], TokenKind::Super);
    CHECK_EQ(k[4], TokenKind::Copy);
    CHECK_EQ(k[5], TokenKind::Is);

    Lexer sl("\"hi\"");
    Token t = sl.next();
    CHECK_EQ(t.kind, TokenKind::LitDStr);
}

static AstPtr parse(const char* src) {
    Lexer lex(src);
    Parser p(lex);
    return p.parseModule();
}

static void test_parser() {
    {
        Lexer lex("start { print 1 }");
        Parser p(lex);
        AstPtr m = p.parseModule();
        CHECK(m != nullptr);
        CHECK(!p.hadError());
        CHECK(m->start != nullptr);
        CHECK(m->start->kind == AstKind::Start || m->start->kind == AstKind::Body);
    }
    {
        Lexer lex("class Box <T> { public int n public create (T v) { } } start { }");
        Parser p(lex);
        AstPtr m = p.parseModule();
        CHECK(m != nullptr);
        CHECK(!p.hadError());
        CHECK(!m->children.empty());
        CHECK_EQ(m->children[0]->kind, AstKind::Class);
        CHECK_EQ(m->children[0]->name, std::string("Box"));
        CHECK(!m->children[0]->nameParts.empty());
        CHECK_EQ(m->children[0]->nameParts[0], std::string("T"));
    }
    {
        Lexer lex("start { print (");
        Parser p(lex);
        p.parseModule();
        CHECK(p.hadError());
    }
    {
        Lexer lex("import foo.bar as B start { }");
        Parser p(lex);
        AstPtr m = p.parseModule();
        CHECK(m != nullptr);
        CHECK(!p.hadError());
        CHECK_EQ(m->children[0]->kind, AstKind::Import);
        CHECK_EQ(m->children[0]->text, std::string("foo.bar"));
        CHECK_EQ(m->children[0]->name, std::string("B"));
    }
}

static void test_semant() {
    {
        Lexer lex("start { print zzz }");
        Parser p(lex);
        AstPtr m = p.parseModule();
        CHECK(m != nullptr);
        CHECK(!p.hadError());
        Semant s;
        s.analyze(m.get());
        CHECK(s.errorCount() > 0);
    }
    {
        Lexer lex("start { int n n = 1 print n }");
        Parser p(lex);
        AstPtr m = p.parseModule();
        CHECK(!p.hadError());
        Semant s;
        s.analyze(m.get());
        CHECK_EQ(s.errorCount(), 0);
    }
    {
        Lexer lex("class Box <T> { public T value public create (T v) { value = v } } start { Box<int> a a = Box<int>.create(1) }");
        Parser p(lex);
        AstPtr m = p.parseModule();
        CHECK(!p.hadError());
        Semant s;
        int e = s.analyze(m.get());
        CHECK_EQ(e, 0);
        CHECK_EQ(s.errorCount(), 0);
    }
}


static void test_lexer_more() {
    auto k = kinds("try except finally raise yield break continue");
    CHECK_EQ(k[0], TokenKind::Try);
    CHECK_EQ(k[1], TokenKind::Except);
    CHECK_EQ(k[2], TokenKind::Finally);
    CHECK_EQ(k[3], TokenKind::Raise);
    CHECK_EQ(k[4], TokenKind::Yield);
    CHECK_EQ(k[5], TokenKind::Break);
    CHECK_EQ(k[6], TokenKind::Continue);

    k = kinds("and or not lt gt lte gte equ neq");
    CHECK_EQ(k[0], TokenKind::And);
    CHECK_EQ(k[1], TokenKind::Or);
    CHECK_EQ(k[2], TokenKind::Not);
    CHECK_EQ(k[3], TokenKind::Lt);
    CHECK_EQ(k[4], TokenKind::Gt);
    CHECK_EQ(k[5], TokenKind::Lte);
    CHECK_EQ(k[6], TokenKind::Gte);
    CHECK_EQ(k[7], TokenKind::Equ);
    CHECK_EQ(k[8], TokenKind::Neq);

    k = kinds("include \"x.simple\"");
    CHECK_EQ(k[0], TokenKind::Include);
    CHECK_EQ(k[1], TokenKind::LitDStr);

    k = kinds("1.5 42 0xFF");
    CHECK_EQ(k[0], TokenKind::LitFloat);
    CHECK_EQ(k[1], TokenKind::LitInt);

    k = kinds("== != <= >= && ||");
    CHECK_EQ(k[0], TokenKind::Eq);
    CHECK_EQ(k[1], TokenKind::Ne);
    CHECK_EQ(k[2], TokenKind::Le);
    CHECK_EQ(k[3], TokenKind::Ge);
    CHECK_EQ(k[4], TokenKind::AndAnd);
    CHECK_EQ(k[5], TokenKind::OrOr);

    Lexer lx("'abc'");
    Token t = lx.next();
    CHECK(t.kind == TokenKind::LitSStr || t.kind == TokenKind::LitDStr);
}

static void test_parser_more() {
    {
        Lexer lex("start { if (1) { print 1 } else { print 2 } }");
        Parser p(lex);
        AstPtr m = p.parseModule();
        CHECK(m && !p.hadError());
        CHECK(m->start != nullptr);
    }
    {
        Lexer lex("start { while (1) { break } }");
        Parser p(lex);
        AstPtr m = p.parseModule();
        CHECK(m && !p.hadError());
    }
    {
        Lexer lex("start { do { continue } while (0) }");
        Parser p(lex);
        AstPtr m = p.parseModule();
        CHECK(m && !p.hadError());
    }
    {
        Lexer lex("start { try { raise (E) } except (E as x) { print x } finally { print 1 } }");
        Parser p(lex);
        AstPtr m = p.parseModule();
        CHECK(m && !p.hadError());
    }
    {
        Lexer lex("namespace N { class C { public create () { } } } start { }");
        Parser p(lex);
        AstPtr m = p.parseModule();
        CHECK(m && !p.hadError());
        CHECK_EQ(m->children[0]->kind, AstKind::Namespace);
        CHECK_EQ(m->children[0]->name, std::string("N"));
    }
    {
        Lexer lex("start { list xs xs = [1, 2] dict d d = { a: 1 } }");
        Parser p(lex);
        AstPtr m = p.parseModule();
        CHECK(m && !p.hadError());
    }
    {
        Lexer lex("start { print x as int }");
        Parser p(lex);
        AstPtr m = p.parseModule();
        CHECK(m && !p.hadError());
    }
    {
        Lexer lex("start { print x is Foo }");
        Parser p(lex);
        AstPtr m = p.parseModule();
        CHECK(m && !p.hadError());
    }
    {
        Lexer lex("nothing foo <T> (T x) { return (x) } start { }");
        Parser p(lex);
        AstPtr m = p.parseModule();
        CHECK(m && !p.hadError());
        CHECK_EQ(m->children[0]->kind, AstKind::Function);
        CHECK(!m->children[0]->nameParts.empty());
    }
}

static void test_semant_more() {
    {
        Lexer lex("start { return (1) }");
        Parser p(lex);
        AstPtr m = p.parseModule();
        Semant s;
        s.analyze(m.get());
        CHECK_EQ(s.errorCount(), 0);
    }
    {
        Lexer lex("int add (int a, int b) { return (a + b) } start { print add(1, 2) }");
        Parser p(lex);
        AstPtr m = p.parseModule();
        CHECK(!p.hadError());
        Semant s;
        s.analyze(m.get());
        CHECK_EQ(s.errorCount(), 0);
    }
    {
        Lexer lex("class A { public create () { } } start { A a a = A.create() print type(a) }");
        Parser p(lex);
        AstPtr m = p.parseModule();
        CHECK(!p.hadError());
        Semant s;
        s.analyze(m.get());
        CHECK_EQ(s.errorCount(), 0);
    }
    {
        Lexer lex("start { Nope x }");
        Parser p(lex);
        AstPtr m = p.parseModule();
        Semant s;
        s.analyze(m.get());
        CHECK(s.errorCount() > 0);
    }
}


static void test_lexer_ops() {
    auto k = kinds("+ - * / % ^ . , : =");
    CHECK_EQ(k[0], TokenKind::Plus);
    CHECK_EQ(k[1], TokenKind::Minus);
    CHECK_EQ(k[2], TokenKind::Star);
    CHECK_EQ(k[3], TokenKind::Slash);
    CHECK_EQ(k[4], TokenKind::Percent);
    CHECK_EQ(k[5], TokenKind::Caret);
    CHECK_EQ(k[6], TokenKind::Dot);
    CHECK_EQ(k[7], TokenKind::Comma);
    CHECK_EQ(k[8], TokenKind::Colon);
    CHECK_EQ(k[9], TokenKind::Assign);

    k = kinds("[] ()");
    CHECK_EQ(k[0], TokenKind::LBracket);
    CHECK_EQ(k[1], TokenKind::RBracket);
    CHECK_EQ(k[2], TokenKind::LParen);
    CHECK_EQ(k[3], TokenKind::RParen);

    k = kinds("public private protected namespace");
    CHECK_EQ(k[0], TokenKind::Public);
    CHECK_EQ(k[1], TokenKind::Private);
    CHECK_EQ(k[2], TokenKind::Protected);
    CHECK_EQ(k[3], TokenKind::Namespace);

    k = kinds("true false");
    CHECK(k[0] == TokenKind::LitBool || k[0] == TokenKind::Identifier);
    CHECK(k[1] == TokenKind::LitBool || k[1] == TokenKind::Identifier);

    Lexer hex("/* block\ncomment */ int");
    Token t = hex.next();
    CHECK_EQ(t.kind, TokenKind::Int);
}

static void test_parser_oop() {
    {
        Lexer lex("class D (public A, public B) { public create () { } public destroy { } } start { }");
        Parser p(lex);
        AstPtr m = p.parseModule();
        CHECK(m && !p.hadError());
        CHECK_EQ(m->children[0]->kind, AstKind::Class);
        CHECK(m->children[0]->bases.size() >= 2);
    }
    {
        Lexer lex("class A { public virtual nothing speak () { return () } } start { }");
        Parser p(lex);
        AstPtr m = p.parseModule();
        CHECK(m && !p.hadError());
    }
    {
        Lexer lex("int A.foo () { return (1) } start { }");
        Parser p(lex);
        AstPtr m = p.parseModule();
        CHECK(m && !p.hadError());
        CHECK_EQ(m->children[0]->kind, AstKind::Function);
        CHECK_EQ(m->children[0]->className, std::string("A"));
        CHECK_EQ(m->children[0]->name, std::string("foo"));
    }
    {
        Lexer lex("start { super A.create() }");
        Parser p(lex);
        AstPtr m = p.parseModule();
        CHECK(m && !p.hadError());
    }
    {
        Lexer lex("start { copy x }");
        Parser p(lex);
        AstPtr m = p.parseModule();
        CHECK(m && !p.hadError());
    }
    {
        Lexer lex("include \"a.simple\" start { }");
        Parser p(lex);
        AstPtr m = p.parseModule();
        CHECK(m && !p.hadError());
        CHECK_EQ(m->children[0]->kind, AstKind::Include);
    }
    {
        Lexer lex("start { inline { int x; } }");
        Parser p(lex);
        AstPtr m = p.parseModule();
        CHECK(m && !p.hadError());
    }
}

static void test_semant_oop() {
    {
        Lexer lex("class A { public create () { } public destroy { } } start { A a a = A.create() }");
        Parser p(lex);
        AstPtr m = p.parseModule();
        CHECK(!p.hadError());
        Semant s;
        s.analyze(m.get());
        CHECK_EQ(s.errorCount(), 0);
    }
    {
        Lexer lex("class A { public create () { } } class B (public A) { public create () { } } start { B b b = B.create() }");
        Parser p(lex);
        AstPtr m = p.parseModule();
        CHECK(!p.hadError());
        Semant s;
        s.analyze(m.get());
        CHECK_EQ(s.errorCount(), 0);
    }
    {
        Lexer lex("nothing show <T> (T x) { return () } start { show<int>(1) }");
        Parser p(lex);
        AstPtr m = p.parseModule();
        CHECK(!p.hadError());
        Semant s;
        s.analyze(m.get());
        CHECK_EQ(s.errorCount(), 0);
    }
    {
        Lexer lex("start { int n n = 1 n = m }");
        Parser p(lex);
        AstPtr m = p.parseModule();
        Semant s;
        s.analyze(m.get());
        CHECK(s.errorCount() > 0);
    }
}


static void test_extra_2026() {
    auto k = kinds("inline type list dict float unsigned nothing");
    CHECK_EQ(k[0], TokenKind::Inline);
    CHECK_EQ(k[1], TokenKind::Type);
    CHECK_EQ(k[2], TokenKind::List);
    CHECK_EQ(k[3], TokenKind::Dict);
    CHECK_EQ(k[4], TokenKind::Float);
    CHECK_EQ(k[5], TokenKind::Unsigned);
    CHECK_EQ(k[6], TokenKind::Nothing);

    k = kinds("if else for while do exit");
    CHECK_EQ(k[0], TokenKind::If);
    CHECK_EQ(k[1], TokenKind::Else);
    CHECK_EQ(k[2], TokenKind::For);
    CHECK_EQ(k[3], TokenKind::While);
    CHECK_EQ(k[4], TokenKind::Do);
    CHECK_EQ(k[5], TokenKind::Exit);

    {
        Lexer lex("start { for (1) { break } }");
        Parser p(lex);
        AstPtr m = p.parseModule();
        CHECK(m && !p.hadError());
    }
    {
        Lexer lex("start { print type(1) }");
        Parser p(lex);
        AstPtr m = p.parseModule();
        CHECK(m && !p.hadError());
        Semant s;
        s.analyze(m.get());
        CHECK_EQ(s.errorCount(), 0);
    }
    {
        Lexer lex("start { yield (1) }");
        Parser p(lex);
        CHECK(p.parseModule() != nullptr);
    }
    {
        Lexer lex("Pair<int, string> x start { }");
        Parser p(lex);
        /* may parse as type + name at module level or error; just must not crash */
        p.parseModule();
        CHECK(true);
    }
}


static void test_batch_736() {
    auto k = kinds("return print raise");
    CHECK_EQ(k[0], TokenKind::Return);
    CHECK_EQ(k[1], TokenKind::Print);
    CHECK_EQ(k[2], TokenKind::Raise);

    {
        Lexer lex("start { exit (0) }");
        Parser p(lex);
        CHECK(p.parseModule() && !p.hadError());
    }
    {
        Lexer lex("start { xs[0] = 1 }");
        Parser p(lex);
        CHECK(p.parseModule() != nullptr);
    }
    {
        Lexer lex("int id (int x) { return (x) } start { print id(8) }");
        Parser p(lex);
        AstPtr m = p.parseModule();
        CHECK(m && !p.hadError());
        Semant s;
        s.analyze(m.get());
        CHECK_EQ(s.errorCount(), 0);
    }
    {
        Lexer lex("class C { public int n public create () { n = 2 } } start { C c c = C.create() print c.n }");
        Parser p(lex);
        AstPtr m = p.parseModule();
        CHECK(m && !p.hadError());
        Semant s;
        s.analyze(m.get());
        CHECK_EQ(s.errorCount(), 0);
    }
}


static void test_batch_737() {
    auto k = kinds("copy is as");
    CHECK_EQ(k[0], TokenKind::Copy);
    CHECK_EQ(k[1], TokenKind::Is);
    CHECK_EQ(k[2], TokenKind::As);

    {
        Lexer lex("start { int n n = (1 + 2) * 3 print n }");
        Parser p(lex);
        CHECK(p.parseModule() && !p.hadError());
    }
    {
        Lexer lex("start { if (0) { print 0 } else (1) { print 1 } else { print 2 } }");
        Parser p(lex);
        CHECK(p.parseModule() && !p.hadError());
    }
    {
        Lexer lex("start { dict d d = { a: 1 } }");
        Parser p(lex);
        AstPtr m = p.parseModule();
        CHECK(m && !p.hadError());
        Semant s;
        s.analyze(m.get());
        CHECK_EQ(s.errorCount(), 0);
    }
}


static void test_null_batch() {
    auto k = kinds("null copy");
    CHECK_EQ(k[0], TokenKind::Null);
    CHECK_EQ(k[1], TokenKind::Copy);
    {
        Lexer lex("start { print null }");
        Parser p(lex);
        AstPtr m = p.parseModule();
        CHECK(m && !p.hadError());
        Semant s;
        s.analyze(m.get());
        CHECK_EQ(s.errorCount(), 0);
    }
    {
        Lexer lex("start { Box b b = null }");
        Parser p(lex);
        CHECK(p.parseModule() != nullptr);
    }
    {
        Lexer lex("null x start { }");
        Parser p(lex);
        p.parseModule();
        CHECK(true);
    }
}


static void test_batch_849() {
    auto k = kinds("null for as virtual");
    CHECK_EQ(k[0], TokenKind::Null);
    CHECK_EQ(k[1], TokenKind::For);
    CHECK_EQ(k[2], TokenKind::As);
    CHECK_EQ(k[3], TokenKind::Virtual);
    {
        Lexer lex("start { for (3 as int i) { print i } }");
        Parser p(lex);
        CHECK(p.parseModule() && !p.hadError());
    }
    {
        Lexer lex("start { list xs xs = [\"a\"] print xs[0] }");
        Parser p(lex);
        CHECK(p.parseModule() && !p.hadError());
    }
    {
        Lexer lex("class A { public virtual nothing f () { return () } public create () { } } class B (public A) { public virtual nothing f () { return () } public create () { } } start { B b A a b = B.create() a = b a.f() }");
        Parser p(lex);
        AstPtr m = p.parseModule();
        CHECK(m && !p.hadError());
        Semant s;
        s.analyze(m.get());
        CHECK_EQ(s.errorCount(), 0);
    }
}

int main() {
    test_lexer();
    test_lexer_more();
    test_parser();
    test_parser_more();
    test_semant();
    test_semant_more();
    test_lexer_ops();
    test_parser_oop();
    test_semant_oop();
    test_extra_2026();
    test_batch_736();
    test_batch_737();
    test_null_batch();
    test_batch_849();
    {
        auto k = kinds("null");
        CHECK_EQ(k[0], TokenKind::Null);
        Lexer lex("start { print null }");
        Parser p(lex);
        CHECK(p.parseModule() && !p.hadError());
    }
    std::printf("unit tests passed=%d failed=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
