#include "ModuleLoader.hpp"
#include "Lexer.hpp"
#include "Parser.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>

ModuleLoader::ModuleLoader(std::string searchDir) : searchDir_(std::move(searchDir)) {
    if (searchDir_.empty()) searchDir_ = ".";
}

std::string ModuleLoader::stripQuotes(const std::string& s) {
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\''))
        return s.substr(1, s.size() - 2);
    return s;
}

std::string ModuleLoader::dirnameOf(const std::string& path) {
    auto pos = path.find_last_of("/\\");
    if (pos == std::string::npos) return ".";
    if (pos == 0) return "/";
    return path.substr(0, pos);
}

std::string ModuleLoader::joinPath(const std::string& dir, const std::string& file) {
    if (file.empty()) return dir;
    if (file[0] == '/' || (file.size() > 1 && file[1] == ':')) return file; // absolute
    if (dir.empty() || dir == ".") return file;
    if (dir.back() == '/' || dir.back() == '\\') return dir + file;
    return dir + "/" + file;
}

std::string ModuleLoader::normalize(const std::string& path) {
    // Simple normalize: collapse "./"
    std::string out;
    for (size_t i = 0; i < path.size(); ) {
        if (path.compare(i, 2, "./") == 0 && (i == 0 || path[i-1] == '/')) {
            i += 2;
            continue;
        }
        out.push_back(path[i++]);
    }
    if (out.empty()) out = ".";
    return out;
}

bool ModuleLoader::readFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

AstPtr ModuleLoader::parseFile(const std::string& path) {
    std::string source;
    if (!readFile(path, source)) {
        error_ = "cannot open '" + path + "'";
        return nullptr;
    }
    Lexer lexer(source);
    Parser parser(lexer);
    AstPtr ast = parser.parseModule();
    if (parser.hadError() || !ast) {
        error_ = "parse failed for '" + path + "'";
        return nullptr;
    }
    return ast;
}

bool ModuleLoader::expandNode(AstNode* parent, size_t index, const std::string& currentDir) {
    if (!parent || index >= parent->children.size()) return true;
    AstNode* node = parent->children[index].get();
    if (!node) return true;

    if (node->kind != AstKind::Include && node->kind != AstKind::Import)
        return true;

    std::string rel = stripQuotes(node->text);
    std::string usePath = resolveImportTarget(rel, currentDir);
    std::string content;
    if (usePath.empty() || !readFile(usePath, content)) {
        error_ = "cannot open include/import '" + rel + "'";
        if (!usePath.empty()) error_ += " (tried " + usePath + ")";
        return false;
    }
    usePath = normalize(usePath);

    if (loading_.count(usePath)) {
        error_ = "circular include/import involving '" + usePath + "'";
        return false;
    }

    loading_.insert(usePath);
    AstPtr imported = parseFile(usePath);
    if (!imported) {
        loading_.erase(usePath);
        return false;
    }

    // Recursively expand the imported module first
    std::string childDir = dirnameOf(usePath);
    if (!expandModule(imported.get(), childDir)) {
        loading_.erase(usePath);
        return false;
    }
    loading_.erase(usePath);

    // Collect items to splice (skip nested start for include of partial files;
    // for a full module with start, keep items only — main file owns start)
    std::vector<AstPtr> items;
    for (auto& ch : imported->children) {
        if (!ch) continue;
        if (ch->kind == AstKind::Include || ch->kind == AstKind::Import)
            continue; // already expanded
        items.push_back(std::move(ch));
    }

    if (node->kind == AstKind::Import && !node->name.empty()) {
        // import "x" as Alias  →  wrap in namespace Alias
        auto ns = std::make_unique<AstNode>(AstKind::Namespace, node->line, node->column);
        ns->name = node->name;
        for (auto& it : items)
            ns->children.push_back(std::move(it));
        parent->children[index] = std::move(ns);
    } else {
        // include or import without alias: splice items in place of the directive
        parent->children.erase(parent->children.begin() + static_cast<long>(index));
        for (size_t i = 0; i < items.size(); ++i) {
            parent->children.insert(
                parent->children.begin() + static_cast<long>(index + i),
                std::move(items[i]));
        }
        // Caller must not advance past spliced range incorrectly — expandModule rescans
    }
    return true;
}

bool ModuleLoader::expandModule(AstNode* module, const std::string& currentDir) {
    if (!module) return true;
    // Rescan from start until no include/import remains
    for (size_t i = 0; i < module->children.size(); ) {
        AstNode* ch = module->children[i].get();
        if (ch && (ch->kind == AstKind::Include || ch->kind == AstKind::Import)) {
            size_t before = module->children.size();
            if (!expandNode(module, i, currentDir)) return false;
            size_t after = module->children.size();
            // If spliced multiple items, re-check from i; if replaced one-for-one (alias ns), advance
            if (after >= before)
                ++i; // replaced with namespace or grew
            // if shrunk/grew, stay at i to process new items
            if (after != before) {
                // stay at i to expand newly inserted includes if any
                continue;
            }
        } else {
            // Recurse into namespaces
            if (ch && ch->kind == AstKind::Namespace) {
                if (!expandModule(ch, currentDir)) return false;
            }
            ++i;
        }
    }
    return true;
}

bool ModuleLoader::resolve(AstNode* module, const std::string& mainPath) {
    error_.clear();
    loading_.clear();
    loaded_.clear();
    std::string dir = dirnameOf(mainPath);
    if (!searchDir_.empty() && searchDir_ != ".")
        dir = searchDir_; // prefer explicit search dir if set
    // Use main file's directory as primary relative base
    dir = dirnameOf(mainPath);
    pkgPaths_.clear();
    loadPathMap("simple.paths");
    loadPathMap(joinPath(dir, "simple.paths"));
    loadPathMap(joinPath(searchDir_, "simple.paths"));
    return expandModule(module, dir);
}

void ModuleLoader::loadPathMap(const std::string& file) {
    std::string text;
    if (!readFile(file, text)) return;
    std::string key, val;
    std::string line;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n') {
            auto hash = line.find('#');
            if (hash != std::string::npos) line = line.substr(0, hash);
            auto eq = line.find('=');
            if (eq != std::string::npos) {
                key = line.substr(0, eq);
                val = line.substr(eq + 1);
                while (!key.empty() && (key.back()==' '||key.back()=='\r')) key.pop_back();
                while (!val.empty() && (val.back()==' '||val.back()=='\r')) val.pop_back();
                size_t ks = 0; while (ks < key.size() && key[ks]==' ') ks++;
                size_t vs = 0; while (vs < val.size() && val[vs]==' ') vs++;
                if (ks < key.size() && vs < val.size())
                    pkgPaths_[key.substr(ks)] = val.substr(vs);
            }
            line.clear();
        } else line.push_back(text[i]);
    }
}

std::string ModuleLoader::resolveImportTarget(const std::string& spec, const std::string& currentDir) {
    if (spec.empty()) return {};
    bool looksFile = spec.find('/') != std::string::npos || spec.find('\\') != std::string::npos
        || spec.size() >= 7 && spec.compare(spec.size()-7, 7, ".simple") == 0;
    if (!looksFile) {
        auto it = pkgPaths_.find(spec);
        if (it != pkgPaths_.end()) return normalize(it->second);
        auto dot = spec.find('.');
        if (dot != std::string::npos) {
            it = pkgPaths_.find(spec.substr(0, dot));
            if (it != pkgPaths_.end()) {
                std::string rest = spec.substr(dot + 1);
                std::string dir = dirnameOf(it->second);
                std::string cand = joinPath(dir, rest + ".simple");
                std::string dummy;
                if (readFile(cand, dummy)) return normalize(cand);
            }
        }
        std::string vendor = joinPath("vendor", spec);
        /* vendor/pkg/ver/src/file.simple via paths file preferred */
    }
    std::string path = normalize(joinPath(currentDir, spec));
    std::string dummy;
    if (readFile(path, dummy)) return path;
    std::string alt = normalize(joinPath(searchDir_, spec));
    if (readFile(alt, dummy)) return alt;
    return path;
}
