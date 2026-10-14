// modules.hpp -- compile-time local import resolution.
//
// A direct port of the original src/modules.art: Arturo's local `import`
// executes a file unscoped at the import position, so for the standalone
// target it is source inclusion, resolved while compiling so the produced
// executable never needs the source tree at run time. The namespace forms
// lower exactly as the original does: `import.lean` binds a dictionary of
// the module's definitions, `import.only:[names]` additionally projects the
// selected names into scope. Cycles and missing files are compile errors.
#pragma once

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "lexer.hpp"  // for CompileError

namespace artc {
namespace modules {

inline std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    auto e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? "" : s.substr(b, e - b + 1);
}
inline bool startsWith(const std::string& s, const std::string& p) { return s.rfind(p, 0) == 0; }
inline bool endsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}
inline std::vector<std::string> splitLines(const std::string& src) {
    std::vector<std::string> lines;
    std::string cur;
    for (char ch : src) {
        if (ch == '\n') { lines.push_back(cur); cur.clear(); }
        else if (ch != '\r') cur += ch;
    }
    lines.push_back(cur);
    return lines;
}
inline std::string joinLines(const std::vector<std::string>& lines) {
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i) out += '\n';
        out += lines[i];
    }
    return out;
}
inline std::string readFileText(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return "";
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
inline std::string quotedPath(const std::string& line) {
    auto a = line.find('"');
    if (a == std::string::npos) return "";
    auto b = line.find('"', a + 1);
    if (b == std::string::npos) return "";
    return line.substr(a + 1, b - a - 1);
}
inline std::string bracedPath(const std::string& line) {
    auto a = line.find('{');
    if (a == std::string::npos) return "";
    auto b = line.find('}', a + 1);
    if (b == std::string::npos) return "";
    return line.substr(a + 1, b - a - 1);
}
// the local path named by a complete import statement, or "" when the line
// is not an import (mirrors moduleImportPath)
inline std::string importPathOf(const std::string& line) {
    std::string clean = trim(line);
    if (clean.find("import.only") != std::string::npos || clean.find("import.lean") != std::string::npos) {
        if (clean.find('"') != std::string::npos) return quotedPath(clean);
        if (clean.find('{') != std::string::npos) return bracedPath(clean);
        return "";
    }
    if (startsWith(clean, "import ./\"") || startsWith(clean, "import \"")) return quotedPath(clean);
    if (startsWith(clean, "import ./{")) return bracedPath(clean);
    if (startsWith(clean, "import relative \"")) return quotedPath(clean);
    if (startsWith(clean, "import relative {")) return bracedPath(clean);
    return "";
}
inline std::vector<std::string> onlyNames(const std::string& line) {
    std::vector<std::string> out;
    auto a = line.find('[');
    auto b = a == std::string::npos ? a : line.find(']', a + 1);
    if (a == std::string::npos || b == std::string::npos) return out;
    std::istringstream ss(line.substr(a + 1, b - a - 1));
    std::string w;
    while (ss >> w) out.push_back(w);
    return out;
}
inline std::string packageEntry(const std::string& infoSource) {
    for (const auto& raw : splitLines(infoSource)) {
        std::string clean = trim(raw);
        if (!startsWith(clean, "entry:")) continue;
        std::string entry;
        if (clean.find('"') != std::string::npos) entry = quotedPath(clean);
        else if (clean.find('{') != std::string::npos) entry = bracedPath(clean);
        else {
            auto colon = clean.find(':');
            if (colon == std::string::npos) continue;
            entry = trim(clean.substr(colon + 1));
            if (!entry.empty() && entry.front() == '\'') entry.erase(entry.begin());
        }
        if (!entry.empty() && !endsWith(entry, ".art")) entry += ".art";
        if (!entry.empty()) return entry;
    }
    return "main.art";
}

class Resolver {
  public:
    std::string resolveFromFile(const std::filesystem::path& path) {
        auto full = std::filesystem::absolute(path).lexically_normal();
        std::string src = readFileText(full);
        if (src.empty()) throw CompileError("compile input not found or empty: " + full.string());
        counter_ = 0;
        return resolve(src, full.parent_path(), {full.string()});
    }

  private:
    int counter_ = 0;

    std::string resolve(const std::string& src, const std::filesystem::path& base,
                        std::vector<std::string> active) {
        std::vector<std::string> out;
        for (const auto& line : splitLines(src)) {
            std::string imported = importPathOf(line);
            if (imported.empty()) { out.push_back(line); continue; }
            bool lean = line.find("import.lean") != std::string::npos;
            bool only = line.find("import.only") != std::string::npos;
            if (lean && only) throw CompileError("compile-time import: combined import.lean.only is not yet supported");
            auto full = (base / imported).lexically_normal();
            if (std::find(active.begin(), active.end(), full.string()) != active.end())
                throw CompileError("compile-time import cycle: " + full.string());
            std::filesystem::path file = full;
            if (std::filesystem::is_directory(full)) {
                std::string entry = "main.art";
                auto info = full / "info.art";
                std::string infoSrc = readFileText(info);
                if (!infoSrc.empty()) entry = packageEntry(infoSrc);
                file = full / entry;
            }
            std::string importedSource = readFileText(file);
            if (importedSource.empty()) {
                auto packageMain = full / "main.art";
                std::string packageSource = readFileText(packageMain);
                if (!packageSource.empty()) {
                    file = packageMain;
                    importedSource = packageSource;
                }
            }
            if (importedSource.empty())
                throw CompileError("compile-time import not found or empty: " + file.string());
            auto nextActive = active;
            nextActive.push_back(file.string());
            std::string expanded = resolve(importedSource, file.parent_path(), nextActive);
            if (!lean && !only) {
                out.push_back(expanded);
            } else if (lean) {
                std::string clean = trim(line);
                std::vector<std::string> replacement;
                auto colon = clean.find(':');
                if (colon != std::string::npos) replacement.push_back(clean.substr(0, colon) + ":");
                replacement.push_back("dictionary [");
                replacement.push_back(expanded);
                replacement.push_back("]");
                out.push_back(joinLines(replacement));
            } else {
                std::string temp = "__module_import_" + std::to_string(++counter_);
                std::vector<std::string> replacement;
                replacement.push_back(temp + ":");
                replacement.push_back("dictionary [");
                replacement.push_back(expanded);
                replacement.push_back("]");
                for (auto name : onlyNames(line)) {
                    if (!name.empty() && name.front() == '\'') name.erase(name.begin());
                    replacement.push_back(name + ":");
                    replacement.push_back(temp + "\\" + name);
                }
                out.push_back(joinLines(replacement));
            }
        }
        return joinLines(out);
    }
};

inline std::string resolveImportsFromFile(const std::filesystem::path& path) {
    Resolver r;
    return r.resolveFromFile(path);
}

}  // namespace modules
}  // namespace artc
