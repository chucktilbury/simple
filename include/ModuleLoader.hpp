#pragma once
#include "Ast.hpp"
#include <string>
#include <unordered_set>
#include <vector>
#include <unordered_map>

class ModuleLoader {
public:
    explicit ModuleLoader(std::string searchDir);

    /* Expand all include / import nodes in-place on the module AST.
       Returns false on error. */
    bool resolve(AstNode* module, const std::string& mainPath);

    const std::string& lastError() const { return error_; }

private:
    std::string searchDir_;
    std::unordered_set<std::string> loading_;   /* cycle detection */
    std::unordered_set<std::string> loaded_;    /* already fully loaded */
    std::string error_;
    std::unordered_map<std::string, std::string> pkgPaths_;
    void loadPathMap(const std::string& file);
    std::string resolveImportTarget(const std::string& spec, const std::string& currentDir);


    std::string stripQuotes(const std::string& s);
    std::string dirnameOf(const std::string& path);
    std::string joinPath(const std::string& dir, const std::string& file);
    std::string normalize(const std::string& path);
    bool readFile(const std::string& path, std::string& out);
    AstPtr parseFile(const std::string& path);
    bool expandNode(AstNode* parent, size_t index, const std::string& currentDir);
    bool expandModule(AstNode* module, const std::string& currentDir);
};
