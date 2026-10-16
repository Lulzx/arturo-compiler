// main.cpp -- arturoc: the optimizing native Arturo compiler (C++26).
//
//   arturoc [options] <in.art> <out>
//
// Fast path (default): lex -> group -> emit compact IR -> optimize -> write
// a native executable made of the prebuilt stub plus an AIR1 payload. No C
// compiler runs, so compilation is a single linear pass over the source.
// C path (--backend=c): the same optimized IR is rendered as C on the
// original runtime's builder API and compiled with cc, for environments that
// want a conventionally compiled binary.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "direct_c.hpp"
#include "emit.hpp"
#include "emit_c.hpp"
#include "ir.hpp"
#include "lexer.hpp"
#include "modules.hpp"
#include "optimizer.hpp"
#include "parser.hpp"
#include "verify.hpp"

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// arturoc's own release version; the compiler targets the pinned
// Arturo 0.10.0 language/runtime (see cpp26/CONTRACT.md).
static constexpr const char* kArturocVersion = "1.0.0";
static constexpr const char* kTargetArturoVersion = "0.10.0";

static double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}
static std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw artc::CompileError("cannot read " + p.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
static void writeAll(const fs::path& p, const std::uint8_t* data, std::size_t n) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) throw artc::CompileError("cannot write " + p.string());
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(n));
    if (!out) throw artc::CompileError("failed writing " + p.string());
}
// Executables are replaced atomically (temp sibling + rename): truncating
// in place can hand the OS a partially written file, and on macOS rapidly
// re-creating the same path has been observed to get the new binary killed
// by stale code-signature validation of the previous inode's contents.
static void writeExecutable(const fs::path& p, const std::uint8_t* data, std::size_t n) {
    fs::path tmp = p.string() + ".tmp-" + std::to_string(static_cast<long>(::getpid()));
    writeAll(tmp, data, n);
    ::chmod(tmp.c_str(), 0755);
    std::error_code ec;
    fs::rename(tmp, p, ec);
    if (ec) {
        fs::remove(tmp, ec);
        throw artc::CompileError("cannot replace " + p.string());
    }
}
static fs::path exeDir() {
#if defined(__APPLE__)
    char buf[4096];
    std::uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) return fs::path(buf).parent_path();
#else
    std::error_code ec;
    auto p = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) return p.parent_path();
#endif
    return fs::current_path();
}
static fs::path findStub() {
    if (const char* e = std::getenv("ARTURO_STUB")) return e;
    auto cand = exeDir() / "arturo-stub";
    if (fs::exists(cand)) return cand;
    return cand;
}
static fs::path findRuntimeDir() {
    if (const char* e = std::getenv("ARTURO_RUNTIME_DIR")) return e;
    for (auto cand : {exeDir() / "../../runtime", exeDir() / "../runtime",
                      fs::path("runtime"), fs::path("cpp26/../runtime")})
        if (fs::exists(cand / "runtime.h")) return cand;
    return exeDir() / "../../runtime";
}
static int runCmd(const std::string& cmd) { return std::system(cmd.c_str()); }

static std::string dumpIR(const air::Program& p) {
    std::ostringstream out;
    for (std::size_t i = 0; i < p.nodes.size(); ++i) {
        const auto& n = p.nodes[i];
        out << i << ": " << air::opName(n.op);
        if (n.name != air::kNone) out << " " << p.str(n.name);
        if (n.con != air::kNone) out << " const#" << n.con;
        if (!n.names.empty()) {
            out << " [";
            for (std::size_t j = 0; j < n.names.size(); ++j)
                out << (j ? " " : "") << p.str(n.names[j]);
            out << "]";
        }
        if (!n.kids.empty()) {
            out << " (";
            for (std::size_t j = 0; j < n.kids.size(); ++j) out << (j ? " " : "") << n.kids[j];
            out << ")";
        }
        out << "\n";
    }
    return out.str();
}

int main(int argc, char** argv) {
    auto boot = Clock::now();
    std::string inPath, outPath, backend = "fast", emitIr, emitC;
    bool optimize = true, stats = false, timing = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto needValue = [&](const std::string& flag) -> std::string {
            if (i + 1 >= argc) throw artc::CompileError("missing value for " + flag);
            return argv[++i];
        };
        try {
            if (a == "-O0") optimize = false;
            else if (a == "-O1" || a == "-O2") optimize = true;
            else if (a == "--backend") backend = needValue(a);
            else if (a.rfind("--backend=", 0) == 0) backend = a.substr(10);
            else if (a == "--emit-ir") emitIr = needValue(a);
            else if (a == "--emit-c") emitC = needValue(a);
            else if (a == "--stats") stats = true;
            else if (a == "--timing") timing = true;
            else if (a == "--dump-ir") emitIr = "-";
            else if (a == "--help" || a == "-h") {
                std::cout << "arturoc " << kArturocVersion << " (targets Arturo " << kTargetArturoVersion << ")\n"
                             "usage: arturoc [-O0|-O1|-O2] [--backend=fast|c|direct] [--emit-ir f] "
                             "[--emit-c f] [--dump-ir] [--stats] [--timing] [--version] <in.art> <out>\n";
                return 0;
            } else if (a == "--version" || a == "-V") {
                std::cout << "arturoc " << kArturocVersion << " (targets Arturo " << kTargetArturoVersion << ")\n";
                return 0;
            } else if (!a.empty() && a[0] == '-') throw artc::CompileError("unknown option: " + a);
            else if (inPath.empty()) inPath = a;
            else if (outPath.empty()) outPath = a;
            else throw artc::CompileError("too many positional arguments");
        } catch (const artc::CompileError& e) {
            std::cerr << "arturoc: " << e.what() << "\n";
            return 1;
        }
    }
    if (inPath.empty() || outPath.empty()) {
        std::cerr << "usage: arturoc [-O0|-O1|-O2] [--backend=fast|c|direct] [--emit-ir f] "
                     "[--emit-c f] [--dump-ir] [--stats] [--timing] <in.art> <out>\n";
        return 1;
    }
    try {
        auto t0 = Clock::now();
        std::string src = artc::modules::resolveImportsFromFile(inPath);
        double readMs = msSince(t0);

        t0 = Clock::now();
        artc::Lexer lexer(src);
        auto tokens = lexer.run();
        double lexMs = msSince(t0);

        t0 = Clock::now();
        artc::Grouper grouper;
        auto grouped = grouper.parseSeq(tokens);
        air::Program prog;
        prog.sourcePath = inPath;
        artc::Emitter emitter(grouper, prog);
        emitter.emitProgram(grouped.roots);
        double frontMs = msSince(t0);
        {
            std::string verr;
            if (!artc::verifyProgram(prog, verr))
                throw artc::CompileError("IR verification failed: " + verr);
        }

        std::size_t nodesBefore = prog.nodes.size();
        artc::OptStats ostat;
        t0 = Clock::now();
        if (optimize) {
            artc::Optimizer opt([&](const std::string& n) { return grouper.isPure(n); });
            ostat = opt.run(prog);
        }
        double optMs = msSince(t0);
        if (optimize) {
            std::string verr;
            if (!artc::verifyProgram(prog, verr))
                throw artc::CompileError("IR verification failed after optimization: " + verr);
        }
        std::size_t irBytes = 0;
        {
            std::vector<std::uint8_t> probe;
            air::writeProgram(prog, probe);
            irBytes = probe.size();
        }

        if (!emitIr.empty()) {
            if (emitIr == "-") std::cout << dumpIR(prog);
            else {
                std::vector<std::uint8_t> bytes;
                air::writeProgram(prog, bytes);
                writeAll(emitIr, bytes.data(), bytes.size());
            }
        }

        t0 = Clock::now();
        if (backend == "fast") {
            fs::path stub = findStub();
            std::string stubBytes = readAll(stub);
            std::vector<std::uint8_t> payload;
            air::writeProgram(prog, payload);
            std::vector<std::uint8_t> out;
            out.insert(out.end(), stubBytes.begin(), stubBytes.end());
            std::uint64_t off = out.size();
            out.insert(out.end(), payload.begin(), payload.end());
            std::uint64_t len = payload.size();
            const char magic[8] = {'A', 'R', 'T', 'C', 'F', 'T', '0', '1'};
            out.insert(out.end(), magic, magic + 8);
            auto put64 = [&](std::uint64_t v) {
                for (int k = 0; k < 8; ++k) out.push_back(static_cast<std::uint8_t>(v >> (8 * k)));
            };
            put64(off); put64(len); put64(0);
            writeExecutable(outPath, out.data(), out.size());
        } else if (backend == "c" || backend == "direct") {
            fs::path rt = findRuntimeDir();
            artc::CEmitter ce(prog);
            std::string csrc;
            if (backend == "direct") {
                artc::DirectEmitter de(prog, ce);
                csrc = de.run();
            } else csrc = ce.run();
            if (!emitC.empty()) writeAll(emitC, reinterpret_cast<const std::uint8_t*>(csrc.data()), csrc.size());
            fs::path cpath = fs::path(outPath + ".arturoc.c");
            fs::path opath = fs::path(outPath + ".arturoc.o");
            writeAll(cpath, reinterpret_cast<const std::uint8_t*>(csrc.data()), csrc.size());
#ifdef __APPLE__
            const char* extra = "";
#else
            const char* extra = "-lcrypto";
#endif
            std::string cc = std::getenv("CC") ? std::getenv("CC") : "cc";
            // Single-quote for the shell, escaping embedded single quotes:
            // output paths are user input and must never become shell syntax.
            auto sh = [](const fs::path& p) {
                std::string s = "'";
                for (char c : p.string()) s += c == '\'' ? "'\\''" : std::string(1, c);
                return s + "'";
            };
            // The generated C and objects are intermediates (the C text is
            // available via --emit-c); remove them on success and failure.
            bool builtRuntime = false;
            auto cleanup = [&]() {
                std::error_code ec;
                fs::remove(cpath, ec);
                fs::remove(opath, ec);
                if (builtRuntime) fs::remove(fs::path(outPath + ".runtime.o"), ec);
            };
            int rc = runCmd(cc + " -O2 -I" + sh(rt) + " -c " + sh(cpath) + " -o " + sh(opath));
            if (rc != 0) { cleanup(); throw artc::CompileError("C compilation failed for " + outPath); }
            fs::path rto = exeDir() / "runtime.o";  // built by cpp26/Makefile from the current runtime.c
            if (!fs::exists(rto)) rto = rt / "runtime.o";
            if (!fs::exists(rto)) {
                rto = fs::path(outPath + ".runtime.o");
                builtRuntime = true;
                rc = runCmd(cc + " -O2 -I" + sh(rt) + " -c " + sh(rt / "runtime.c") + " -o " + sh(rto));
                if (rc != 0) { cleanup(); throw artc::CompileError("runtime compilation failed"); }
            }
            rc = runCmd(cc + " -O2 " + sh(opath) + " " + sh(rto) + " -lm " + extra + " -o " + sh(fs::path(outPath)));
            if (rc != 0) { cleanup(); throw artc::CompileError("link failed for " + outPath); }
            cleanup();
            ::chmod(outPath.c_str(), 0755);
        } else {
            throw artc::CompileError("unknown backend: " + backend + " (expected fast, c, or direct)");
        }
        double backMs = msSince(t0);

        if (stats) {
            std::cerr << "arturoc: nodes " << nodesBefore << " -> " << prog.nodes.size()
                      << ", consts " << prog.consts.size() << ", strings " << prog.strings.size()
                      << ", AIR bytes " << irBytes << ", source bytes " << src.size()
                      << ", folded " << ostat.folded << ", branches " << ostat.branchesFolded
                      << ", dead " << ostat.deadRemoved << "\n";
        }
        if (timing) {
            std::cerr << "arturoc: read " << readMs << "ms lex " << lexMs << "ms front " << frontMs
                      << "ms opt " << optMs << "ms backend " << backMs << "ms total " << msSince(boot)
                      << "ms\n";
        }
        return 0;
    } catch (const artc::CompileError& e) {
        std::cerr << "arturoc: " << inPath << ": " << e.what() << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "arturoc: " << inPath << ": " << e.what() << "\n";
        return 1;
    }
}
