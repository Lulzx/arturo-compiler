// ir.cpp -- AIR1 binary codec: unsigned LEB128 varints throughout, so the
// common node (one-byte opcode, two small pool indices) costs ~4 bytes.
#include "ir.hpp"

#include <cstring>

namespace air {
namespace {

void putVar(std::vector<std::uint8_t>& out, std::uint64_t v) {
    do {
        std::uint8_t byte = static_cast<std::uint8_t>(v & 0x7F);
        v >>= 7;
        if (v) byte |= 0x80;
        out.push_back(byte);
    } while (v);
}
void putStr(std::vector<std::uint8_t>& out, const std::string& s) {
    putVar(out, s.size());
    out.insert(out.end(), s.begin(), s.end());
}
void putConst(std::vector<std::uint8_t>& out, const Const& c) {
    out.push_back(static_cast<std::uint8_t>(c.kind));
    switch (c.kind) {
        case CKind::Int: {
            std::uint64_t z = (static_cast<std::uint64_t>(c.i) << 1) ^
                              (c.i < 0 ? 0xFFFF'FFFF'FFFF'FFFFull : 0ull);
            putVar(out, z);
            break;
        }
        case CKind::Float: {
            std::uint64_t bits;
            std::memcpy(&bits, &c.f, 8);
            for (int k = 0; k < 8; ++k) out.push_back(static_cast<std::uint8_t>(bits >> (8 * k)));
            break;
        }
        case CKind::Bool: out.push_back(c.b ? 1 : 0); break;
        case CKind::Quantity: {
            std::uint64_t z = (static_cast<std::uint64_t>(c.i) << 1) ^
                              (c.i < 0 ? 0xFFFF'FFFF'FFFF'FFFFull : 0ull);
            putVar(out, z);
            std::uint64_t bits;
            std::memcpy(&bits, &c.f, 8);
            for (int k = 0; k < 8; ++k) out.push_back(static_cast<std::uint8_t>(bits >> (8 * k)));
            out.push_back(c.b ? 1 : 0);
            putStr(out, c.s);
            break;
        }
        case CKind::Null: break;
        case CKind::Block:
        case CKind::Inline:
        case CKind::PathLiteral:
            putVar(out, c.items.size());
            for (const auto& it : c.items) putConst(out, it);
            break;
        default: putStr(out, c.s); break;
    }
}

struct Reader {
    const std::uint8_t* p;
    std::size_t n, at = 0;
    bool var(std::uint64_t& v) {
        v = 0; int shift = 0;
        while (at < n && shift < 64) {
            std::uint8_t byte = p[at++];
            v |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
            if (!(byte & 0x80)) return true;
            shift += 7;
        }
        return false;
    }
    bool str(std::string& s) {
        std::uint64_t len;
        if (!var(len) || at + len > n) return false;
        s.assign(reinterpret_cast<const char*>(p + at), static_cast<std::size_t>(len));
        at += static_cast<std::size_t>(len);
        return true;
    }
    bool byte(std::uint8_t& b) {
        if (at >= n) return false;
        b = p[at++];
        return true;
    }
};

bool getConst(Reader& r, Const& c, int depth) {
    if (depth > 64) return false;
    std::uint8_t kind;
    if (!r.byte(kind)) return false;
    if (kind > static_cast<std::uint8_t>(CKind::Rational)) return false;
    c.kind = static_cast<CKind>(kind);
    switch (c.kind) {
        case CKind::Int: {
            std::uint64_t z;
            if (!r.var(z)) return false;
            c.i = static_cast<std::int64_t>((z >> 1) ^ (0 - (z & 1)));
            return true;
        }
        case CKind::Float: {
            if (r.at + 8 > r.n) return false;
            std::uint64_t bits = 0;
            for (int k = 0; k < 8; ++k) bits |= static_cast<std::uint64_t>(r.p[r.at++]) << (8 * k);
            std::memcpy(&c.f, &bits, 8);
            return true;
        }
        case CKind::Bool: {
            std::uint8_t b;
            if (!r.byte(b)) return false;
            c.b = b != 0;
            return true;
        }
        case CKind::Quantity: {
            std::uint64_t z;
            if (!r.var(z)) return false;
            c.i = static_cast<std::int64_t>((z >> 1) ^ (0 - (z & 1)));
            if (r.at + 8 > r.n) return false;
            std::uint64_t bits = 0;
            for (int k = 0; k < 8; ++k) bits |= static_cast<std::uint64_t>(r.p[r.at++]) << (8 * k);
            std::memcpy(&c.f, &bits, 8);
            std::uint8_t b;
            if (!r.byte(b)) return false;
            c.b = b != 0;
            return r.str(c.s);
        }
        case CKind::Null: return true;
        case CKind::Block:
        case CKind::Inline:
        case CKind::PathLiteral: {
            std::uint64_t count;
            if (!r.var(count) || count > 1'000'000) return false;
            c.items.resize(static_cast<std::size_t>(count));
            for (auto& it : c.items) if (!getConst(r, it, depth + 1)) return false;
            return true;
        }
        default: return r.str(c.s);
    }
}

}  // namespace

const char* opName(Op op) {
    static constexpr const char* names[] = {
        "__seq", "const", "load", "intrinsic", "define", "let", "call", "function",
        "method", "params", "block", "if", "unless", "switch", "when", "using",
        "try", "throws?", "ensure", "do", "return", "while", "until", "loop",
        "higher", "range", "dictionary", "passthrough", "break", "continue",
        "constructor", "is",
    };
    auto i = static_cast<std::size_t>(op);
    return i < sizeof(names) / sizeof(*names) ? names[i] : "?";
}

void writeProgram(const Program& p, std::vector<std::uint8_t>& out) {
    out.insert(out.end(), {'A', 'I', 'R', '1'});
    putVar(out, 2);  // format version (2: per-node source lines)
    putStr(out, p.sourcePath);
    putVar(out, p.strings.size());
    for (const auto& s : p.strings) putStr(out, s);
    putVar(out, p.consts.size());
    for (const auto& c : p.consts) putConst(out, c);
    putVar(out, p.nodes.size());
    for (const auto& nd : p.nodes) {
        out.push_back(static_cast<std::uint8_t>(nd.op));
        putVar(out, nd.name == kNone ? 0 : static_cast<std::uint64_t>(nd.name) + 1);
        putVar(out, nd.con == kNone ? 0 : static_cast<std::uint64_t>(nd.con) + 1);
        putVar(out, nd.aux);
        putVar(out, nd.kids.size());
        for (auto k : nd.kids) putVar(out, k);
        putVar(out, nd.attrs.size());
        for (auto& a : nd.attrs) { putVar(out, a.first); putVar(out, a.second); }
        putVar(out, nd.names.size());
        for (auto nm : nd.names) putVar(out, nm);
        putVar(out, nd.line);
    }
    putVar(out, p.root == kNone ? 0 : static_cast<std::uint64_t>(p.root) + 1);
}

bool readProgram(const std::uint8_t* data, std::size_t size, Program& p, std::string& err) {
    if (size < 4 || std::memcmp(data, "AIR1", 4) != 0) { err = "bad AIR1 magic"; return false; }
    Reader r{data + 4, size - 4, 0};
    std::uint64_t v;
    if (!r.var(v) || (v != 1 && v != 2)) { err = "unsupported AIR version"; return false; }
    const bool hasLines = v == 2;
    if (!r.str(p.sourcePath)) { err = "truncated source path"; return false; }
    auto count = [&](std::uint64_t& c) { return r.var(c) && c <= 10'000'000; };
    if (!count(v)) { err = "bad string pool"; return false; }
    p.strings.resize(static_cast<std::size_t>(v));
    for (auto& s : p.strings) if (!r.str(s)) { err = "truncated string pool"; return false; }
    for (std::uint32_t i = 0; i < p.strings.size(); ++i) p.stringIndex.emplace(p.strings[i], i);
    if (!count(v)) { err = "bad constant pool"; return false; }
    p.consts.resize(static_cast<std::size_t>(v));
    for (auto& c : p.consts) if (!getConst(r, c, 0)) { err = "truncated constant pool"; return false; }
    if (!count(v)) { err = "bad node pool"; return false; }
    p.nodes.resize(static_cast<std::size_t>(v));
    for (auto& nd : p.nodes) {
        std::uint8_t opb; std::uint64_t x;
        if (!r.byte(opb)) { err = "truncated node"; return false; }
        if (opb > static_cast<std::uint8_t>(Op::Is)) { err = "unknown opcode"; return false; }
        nd.op = static_cast<Op>(opb);
        if (!r.var(x)) return false; nd.name = x == 0 ? kNone : static_cast<std::uint32_t>(x - 1);
        if (!r.var(x)) return false; nd.con = x == 0 ? kNone : static_cast<std::uint32_t>(x - 1);
        if (!r.var(x)) return false; nd.aux = static_cast<std::uint32_t>(x);
        if (!count(x)) return false;
        nd.kids.resize(static_cast<std::size_t>(x));
        for (auto& k : nd.kids) { if (!r.var(v)) return false; k = static_cast<std::uint32_t>(v); }
        if (!count(x)) return false;
        nd.attrs.resize(static_cast<std::size_t>(x));
        for (auto& a : nd.attrs) {
            if (!r.var(v)) return false; a.first = static_cast<std::uint32_t>(v);
            if (!r.var(v)) return false; a.second = static_cast<std::uint32_t>(v);
        }
        if (!count(x)) return false;
        nd.names.resize(static_cast<std::size_t>(x));
        for (auto& nm : nd.names) { if (!r.var(v)) return false; nm = static_cast<std::uint32_t>(v); }
        if (hasLines) { if (!r.var(v)) return false; nd.line = static_cast<std::uint32_t>(v); }
    }
    if (!r.var(v)) { err = "missing root"; return false; }
    p.root = v == 0 ? kNone : static_cast<std::uint32_t>(v - 1);
    if (p.root != kNone && p.root >= p.nodes.size()) { err = "root out of range"; return false; }
    // Pool indices are dereferenced without further checks by the stub's IR
    // builder, so validate every reference here, at decode time: a corrupt
    // or hand-crafted payload must fail with an error, never a crash.
    for (std::size_t i = 0; i < p.nodes.size(); ++i) {
        const auto& nd = p.nodes[i];
        auto oob = [&](const char* what) {
            err = "node " + std::to_string(i) + ": " + what + " out of range";
            return false;
        };
        if (nd.name != kNone && nd.name >= p.strings.size()) return oob("name index");
        if (nd.con != kNone && nd.con >= p.consts.size()) return oob("constant index");
        for (auto k : nd.kids) if (k >= p.nodes.size()) return oob("child index");
        for (auto& a : nd.attrs) {
            if (a.first >= p.strings.size()) return oob("attribute name");
            if (a.second >= p.nodes.size()) return oob("attribute value");
        }
        for (auto nm : nd.names) if (nm >= p.strings.size()) return oob("parameter name");
    }
    return true;
}

}  // namespace air
