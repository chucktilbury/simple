#include "Lexer.hpp"
#include "Parser.hpp"
#include "Semant.hpp"
#include "Codegen.hpp"
#include "Token.hpp"
#include "ModuleLoader.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

static bool readFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "cannot open " << path << "\n";
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

static void usage(const char* argv0) {
    std::cerr
        << "Usage: " << argv0 << " [options] <file.simple>\n"
        << "Options:\n"
        << "  -o FILE         Output path (default: a.out / out.ll / out.o)\n"
        << "  --emit-llvm     Write LLVM IR (.ll)\n"
        << "  -c              Write native object file (.o)\n"
        << "  -g, --debug-info  Emit DWARF debug info for GDB (default: off)\n"
        << "  --no-debug-info   Disable DWARF debug info\n"
        << "  -v, --verbose     Print compiler phase diagnostics\n"
        << "  --dump-tokens     Dump lexer tokens to stderr\n"
        << "  --dump-ast        Dump high-level AST summary\n"
        << "  --no-semant       Skip semantic analysis\n"
        << "  --extra-obj FILE  Link extra .o when building an executable (repeatable)\n"
        << "  -h, --help        Show this help\n";
}

static void dumpTokens(const std::string& source) {
    Lexer lex(source);
    for (;;) {
        Token t = lex.next();
        std::cerr << t.line << ":" << t.column << "  "
                  << tokenName(t.kind) << "  \"" << t.lexeme << "\"\n";
        if (t.kind == TokenKind::Eof) break;
    }
}

static void dumpAstSummary(AstNode* n, int indent = 0) {
    if (!n) return;
    auto pad = std::string(indent * 2, ' ');
    const char* kind = "?";
    switch (n->kind) {
        case AstKind::Module: kind = "Module"; break;
        case AstKind::Start: kind = "Start"; break;
        case AstKind::Namespace: kind = "Namespace"; break;
        case AstKind::Class: kind = "Class"; break;
        case AstKind::Function: kind = "Function"; break;
        case AstKind::Constructor: kind = "Constructor"; break;
        case AstKind::Destructor: kind = "Destructor"; break;
        case AstKind::DataDecl: kind = "DataDecl"; break;
        case AstKind::Body: kind = "Body"; break;
        case AstKind::If: kind = "If"; break;
        case AstKind::While: kind = "While"; break;
        case AstKind::Try: kind = "Try"; break;
        case AstKind::Except: kind = "Except"; break;
        case AstKind::Raise: kind = "Raise"; break;
        case AstKind::Return: kind = "Return"; break;
        case AstKind::Assignment: kind = "Assignment"; break;
        default: kind = "Node"; break;
    }
    std::cerr << pad << kind;
    if (!n->name.empty()) std::cerr << " " << n->name;
    if (!n->bases.empty()) {
        std::cerr << " bases=[";
        for (size_t i = 0; i < n->bases.size(); ++i) {
            if (i) std::cerr << ",";
            std::cerr << n->bases[i];
        }
        std::cerr << "]";
    }
    std::cerr << "  @" << n->line << ":" << n->column << "\n";
    for (auto& c : n->children) dumpAstSummary(c.get(), indent + 1);
    if (n->body) dumpAstSummary(n->body.get(), indent + 1);
    if (n->start) dumpAstSummary(n->start.get(), indent + 1);
    for (auto& c : n->params) dumpAstSummary(c.get(), indent + 1);
}

int main(int argc, char** argv) {
    bool emitLLVM = false;
    bool emitObj = false;
    bool dumpAst = false;
    bool dumpTok = false;
    bool verbose = false;
    bool skipSemant = false;
    bool debugInfo = false;  // DWARF off by default
    std::string input;
    std::string output;
    std::vector<std::string> extraObjs;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--emit-llvm") emitLLVM = true;
        else if (a == "-c" || a == "--emit-obj") emitObj = true;
        else if (a == "--ast" || a == "--dump-ast") dumpAst = true;
        else if (a == "--dump-tokens") dumpTok = true;
        else if (a == "-v" || a == "--verbose") verbose = true;
        else if (a == "-g" || a == "--debug-info") debugInfo = true;
        else if (a == "--no-debug-info") debugInfo = false;
        else if (a == "--no-semant") skipSemant = true;
        else if (a == "-o" && i + 1 < argc) output = argv[++i];
        else if ((a == "--extra-obj" || a == "-extra-obj") && i + 1 < argc)
            extraObjs.push_back(argv[++i]);
        else if (a.rfind("--extra-obj=", 0) == 0)
            extraObjs.push_back(a.substr(12));
        else if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
        else if (!a.empty() && a[0] != '-') input = a;
        else {
            std::cerr << "unknown option: " << a << "\n";
            usage(argv[0]);
            return 1;
        }
    }

    if (input.empty()) {
        usage(argv[0]);
        return 1;
    }

    if (verbose)
        std::cerr << "[verbose] reading " << input << "\n";

    std::string source;
    if (!readFile(input, source)) return 1;

    if (verbose)
        std::cerr << "[verbose] source size: " << source.size() << " bytes\n";

    if (dumpTok) {
        if (verbose) std::cerr << "[verbose] --- tokens ---\n";
        dumpTokens(source);
    }

    if (verbose) std::cerr << "[verbose] parsing...\n";
    Lexer lexer(source);
    Parser parser(lexer);
    AstPtr ast = parser.parseModule();
    if (parser.hadError()) {
        std::cerr << "parse failed (" << parser.errorCount() << " errors)\n";
        return 1;
    }
    if (verbose) std::cerr << "[verbose] parse OK\n";

    if (verbose) std::cerr << "[verbose] resolving include/import...\n";
    ModuleLoader loader(".");
    if (!loader.resolve(ast.get(), input)) {
        std::cerr << "module error: " << loader.lastError() << "\n";
        return 1;
    }
    if (verbose) std::cerr << "[verbose] modules resolved\n";

    if (dumpAst) {
        if (verbose) std::cerr << "[verbose] --- AST ---\n";
        dumpAstSummary(ast.get());
    }

    if (!skipSemant) {
        if (verbose) std::cerr << "[verbose] semantic analysis...\n";
        Semant sem;
        if (sem.analyze(ast.get())) {
            std::cerr << "semantic analysis failed (" << sem.errorCount() << " errors)\n";
            return 1;
        }
        if (verbose) std::cerr << "[verbose] semantic analysis OK\n";
    } else if (verbose) {
        std::cerr << "[verbose] semantic analysis skipped\n";
    }

    if (verbose) {
        std::cerr << "[verbose] codegen (debug-info="
                  << (debugInfo ? "on" : "off") << ")...\n";
    }
    Codegen cg("simple_module", input);
    cg.setDebugInfo(debugInfo);
    for (auto& e : extraObjs) cg.addExtraObj(e);
    if (cg.emit(ast.get())) {
        std::cerr << "code generation failed\n";
        return 1;
    }
    if (verbose) std::cerr << "[verbose] codegen OK\n";

    if (emitLLVM) {
        if (output.empty()) output = "out.ll";
        if (cg.writeIR(output)) return 1;
        std::cerr << "wrote LLVM IR: " << output << "\n";
    } else if (emitObj) {
        if (output.empty()) output = "out.o";
        if (cg.writeObject(output)) return 1;
        std::cerr << "wrote object file: " << output << "\n";
    } else {
        if (output.empty()) output = "a.out";
        if (cg.compileExecutable(output)) return 1;
        std::cerr << "wrote executable: " << output << "\n";
    }

    if (verbose) std::cerr << "[verbose] done\n";
    return 0;
}
