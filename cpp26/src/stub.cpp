// stub.cpp -- the fast-path native runtime executable.
//
// `arturoc` produces a native executable by copying this stub and appending
// one AIR1 payload plus a 32-byte footer; there is no C compilation and no
// linking at program-compile time. The stub decodes the compact IR into the
// original runtime's IR tree and runs it with runNode, so execution semantics
// are the runtime's own -- the same code the original C backend links.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "ir.hpp"

extern "C" {
#include "runtime.h"
}

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
#else
#include <unistd.h>
#endif

namespace {

air::Program g_prog;

Value constValue(const air::Const& c) {
    switch (c.kind) {
        case air::CKind::Null: return v_null();
        case air::CKind::Int: return v_int(static_cast<long>(c.i));
        case air::CKind::BigInt: return v_bigint_text(c.s.c_str());
        case air::CKind::Float: return v_float(c.f);
        case air::CKind::Str: return v_str(c.s.c_str());
        case air::CKind::Char: return v_char_text(c.s.c_str());
        case air::CKind::Bool: return v_bool(c.b ? 1 : 0);
        case air::CKind::Word: return v_token(V_WORD, c.s.c_str());
        case air::CKind::Literal: return v_token(V_LITERAL, c.s.c_str());
        case air::CKind::Symbol: return v_token(V_SYMBOL, c.s.c_str());
        case air::CKind::SymbolLiteral: return v_token(V_SYMBOLLITERAL, c.s.c_str());
        case air::CKind::Type: return v_token(V_TYPE, (":" + c.s).c_str());
        case air::CKind::Label: return v_str(c.s.c_str());
        case air::CKind::Quantity:
            return c.b ? v_quantity_int(static_cast<long>(c.i), c.s.c_str())
                       : v_quantity(c.f, c.s.c_str());
        case air::CKind::Unit: return v_unit(c.s.c_str());
        case air::CKind::Version: return v_version(c.s.c_str());
        case air::CKind::Color: return v_color_hex(c.s.c_str());
        case air::CKind::Regex: return v_token(V_REGEX, c.s.c_str());
        case air::CKind::Attribute: return v_token(V_ATTRIBUTE, c.s.c_str());
        case air::CKind::AttributeLabel: return v_token(V_ATTRIBUTELABEL, c.s.c_str());
        case air::CKind::PathLiteral: {
            std::vector<char*> segs;
            for (const auto& s : c.items) segs.push_back(const_cast<char*>(s.s.c_str()));
            return v_path(segs.data(), static_cast<int>(segs.size()));
        }
        case air::CKind::Block:
        case air::CKind::Inline: {
            // v_block takes ownership of the array itself, so it must be heap
            // allocated -- never a pointer into a stack vector
            auto** items = static_cast<Value**>(
                std::malloc(sizeof(Value*) * (c.items.empty() ? 1 : c.items.size())));
            for (std::size_t i = 0; i < c.items.size(); ++i) {
                auto* v = static_cast<Value*>(std::malloc(sizeof(Value)));
                *v = constValue(c.items[i]);
                items[i] = v;
            }
            return v_block(items, static_cast<int>(c.items.size()));
        }
    }
    return v_null();
}

const char* runtimeOpName(air::Op op) {
    switch (op) {
        case air::Op::Seq: return "__seq";
        case air::Op::Block: return "block";
        case air::Op::Break: return "break";
        case air::Op::Continue: return "continue";
        case air::Op::Dictionary: return "dictionary";
        case air::Op::Do: return "do";
        case air::Op::Ensure: return "ensure";
        case air::Op::Function: return "function";
        case air::Op::Method: return "method";
        case air::Op::Constructor: return "constructor";
        case air::Op::Is: return "is";
        case air::Op::If: return "if";
        case air::Op::Unless: return "unless";
        case air::Op::Switch: return "switch";
        case air::Op::When: return "when";
        case air::Op::Using: return "using";
        case air::Op::Try: return "try";
        case air::Op::ThrowsQ: return "throws?";
        case air::Op::While: return "while";
        case air::Op::Until: return "until";
        case air::Op::Loop: return "loop";
        case air::Op::Range: return "range";
        case air::Op::Return: return "return";
        default: return "passthrough";
    }
}

IR* build(std::uint32_t id);
IR** buildList(const std::vector<std::uint32_t>& kids, int& n) {
    n = static_cast<int>(kids.size());
    if (kids.empty()) return nullptr;
    auto** arr = static_cast<IR**>(std::malloc(sizeof(IR*) * kids.size()));
    for (std::size_t i = 0; i < kids.size(); ++i) arr[i] = build(kids[i]);
    return arr;
}
IR* buildParams(const air::Node& paramsNode, bool asLoads) {
    std::vector<IR*> items;
    for (auto nameId : paramsNode.names) {
        const std::string& nm = g_prog.str(nameId);
        items.push_back(asLoads ? ir_load(nm.c_str())
                                : ir_const(v_str(nm.c_str())));
    }
    return ir_block(items.data(), static_cast<int>(items.size()));
}
IR* withAttrs(IR* node, const air::Node& n) {
    if (n.attrs.empty()) return node;
    std::vector<const char*> names;
    std::vector<IR*> values;
    for (auto& a : n.attrs) {
        names.push_back(g_prog.str(a.first).c_str());
        values.push_back(build(a.second));
    }
    return ir_attrs(node, names.data(), values.data(), static_cast<int>(names.size()));
}

IR* build(std::uint32_t id) {
    const air::Node& n = g_prog.nodes[id];
    switch (n.op) {
        case air::Op::Const: return ir_const(constValue(g_prog.consts[n.con]));
        case air::Op::Passthrough: {
            const auto& c = g_prog.consts[n.con];
            if (c.kind == air::CKind::PathLiteral) return ir_passthrough(constValue(c));
            return ir_passthrough(constValue(c));
        }
        case air::Op::Load: return ir_load(g_prog.str(n.name).c_str());
        case air::Op::Intrinsic: return ir_word(g_prog.str(n.name).c_str());
        case air::Op::Define: return ir_define(g_prog.str(n.name).c_str(), build(n.kids[0]));
        case air::Op::Let: return ir_let(g_prog.str(n.name).c_str(), build(n.kids[0]));
        case air::Op::Call: {
            IR* fn;
            const auto& fnNode = g_prog.nodes[n.kids[0]];
            if (fnNode.op == air::Op::Intrinsic) fn = ir_intrinsic(g_prog.str(fnNode.name).c_str());
            else fn = build(n.kids[0]);
            std::vector<IR*> args;
            for (std::size_t i = 1; i < n.kids.size(); ++i) args.push_back(build(n.kids[i]));
            IR* call = args.empty() && n.attrs.empty()
                           ? ir_call(fn, nullptr, 0)
                           : ir_call(fn, args.data(), static_cast<int>(args.size()));
            return withAttrs(call, n);
        }
        case air::Op::Function: {
            IR* params = buildParams(g_prog.nodes[n.kids[0]], true);
            std::vector<IR*> body;
            for (std::size_t i = 1; i < n.kids.size(); ++i) body.push_back(build(n.kids[i]));
            return withAttrs(ir_fn(params, body.data(), static_cast<int>(body.size())), n);
        }
        case air::Op::Method: {
            int cnt = 0;
            std::vector<IR*> all;
            all.push_back(buildParams(g_prog.nodes[n.kids[0]], true));
            for (std::size_t i = 1; i < n.kids.size(); ++i) all.push_back(build(n.kids[i]));
            cnt = static_cast<int>(all.size());
            return ir_op("method", all.data(), cnt);
        }
        case air::Op::Params: return buildParams(n, true);
        case air::Op::Higher: {
            IR* coll = build(n.kids[0]);
            IR* params = buildParams(g_prog.nodes[n.kids[1]], false);
            // runtime higher actions receive the body as a __seq node
            IR* body = build(n.kids[2]);
            std::vector<const char*> names;
            std::vector<IR*> values;
            for (auto& a : n.attrs) {
                names.push_back(g_prog.str(a.first).c_str());
                values.push_back(build(a.second));
            }
            return ir_higher(g_prog.str(n.name).c_str(), coll, params, body,
                             names.empty() ? nullptr : names.data(),
                             values.empty() ? nullptr : values.data(),
                             static_cast<int>(names.size()));
        }
        case air::Op::Loop: {
            int cnt = 0;
            IR** arr = buildList({n.kids[0], n.kids[2]}, cnt);
            // build params as the middle argument manually:
            IR* args[3] = {arr[0], buildParams(g_prog.nodes[n.kids[1]], false), arr[1]};
            std::free(arr);
            IR* node = ir_op("loop", args, 3);
            return withAttrs(node, n);
        }
        case air::Op::Constructor: {
            IR* params = buildParams(g_prog.nodes[n.kids[0]], false);
            return ir_op("constructor", &params, 1);
        }
        default: {
            int cnt = 0;
            IR** arr = buildList(n.kids, cnt);
            IR* node = ir_op(runtimeOpName(n.op), arr, cnt);
            std::free(arr);
            return withAttrs(node, n);
        }
    }
}

std::string selfPath(const char* argv0) {
#if defined(__APPLE__)
    char buf[4096];
    std::uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) return buf;
    return argv0 ? argv0 : "";
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) { buf[n] = 0; return buf; }
    return argv0 ? argv0 : "";
#endif
}

bool readFile(const std::string& path, std::vector<std::uint8_t>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size < 0) { std::fclose(f); return false; }
    out.resize(static_cast<std::size_t>(size));
    bool ok = size == 0 || std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::uint8_t> payload;
    std::string exe = selfPath(argc > 0 ? argv[0] : nullptr);
    std::vector<std::uint8_t> self;
    bool havePayload = false;
    if (readFile(exe, self) && self.size() >= 32) {
        const auto* foot = self.data() + self.size() - 32;
        if (std::memcmp(foot, "ARTCFT01", 8) == 0) {
            std::uint64_t off, len;
            std::memcpy(&off, foot + 8, 8);
            std::memcpy(&len, foot + 16, 8);
            if (off + len <= self.size() - 32) {
                payload.assign(self.begin() + static_cast<long>(off),
                               self.begin() + static_cast<long>(off + len));
                havePayload = true;
            }
        }
    }
    int argShift = 0;
    if (!havePayload && argc >= 2) {
        std::vector<std::uint8_t> file;
        if (readFile(argv[1], file) && file.size() >= 4 && std::memcmp(file.data(), "AIR1", 4) == 0) {
            payload = std::move(file);
            havePayload = true;
            argShift = 1;
        }
    }
    if (!havePayload) {
        std::fprintf(stderr, "arturo-stub: no AIR1 payload (this is the compiler's runtime stub; "
                             "run a compiled program or pass a .air file)\n");
        return 1;
    }
    std::string err;
    if (!air::readProgram(payload.data(), payload.size(), g_prog, err)) {
        std::fprintf(stderr, "arturo-stub: %s\n", err.c_str());
        return 1;
    }
    runtime_set_args(argc - argShift, argv + argShift);
    runtime_set_source(g_prog.sourcePath.c_str());
    Env* e = env_new(nullptr);
    IR* root = build(g_prog.root);
    (void)runNode(e, root);
    return 0;
}
