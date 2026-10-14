// ir.hpp -- the compact IR: the stable contract between the C++26 front-end,
// the optimizer, and both backends (fast stub payload and emitted C).
//
// Where the original compiler represents IR as Arturo dictionaries
// (#[op: "call" fn: ... args: ...]), this IR is three flat pools -- interned
// strings, deduplicated constants, and nodes whose children are pool indices.
// A node is one byte of opcode plus small integers; there are no per-node
// heap dictionaries, hash lookups, or string comparisons after interning.
// The binary form ("AIR1") is a varint encoding of exactly these pools, so
// compiling to the fast native executable is a single linear write.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace air {

enum class Op : std::uint8_t {
    Seq,        // kids = statements, last value wins (runtime __seq)
    Const,      // con = constant pool index
    Load,       // name = variable to load
    Intrinsic,  // name = primitive; call callee or bare-word value
    Define,     // name, kids[0] = expr  (label binding, `x: expr`)
    Let,        // name, kids[0] = expr  (rebind nearest, `let 'x expr`)
    Call,       // kids[0] = callee, kids[1..] = args, attrs attached
    Function,   // kids[0] = Params, kids[1..] = body statements
    Method,     // like Function; `this` binding is a runtime concern
    Params,     // names = parameter names (aux list below)
    Block,      // kids = element nodes of an evaluated block value @[...]
                // or a raw block value [...] (see `raw` flag on the node)
    If,         // kids = [cond, yesSeq, (noSeq)]
    Unless,     // kids = [cond, bodySeq]
    Switch,     // kids = [cond, yesSeq, noSeq]
    When,       // kids = [cond0, body0, cond1, body1, ...]
    Using,      // kids = [value, bodySeq]
    Try,        // kids = [bodySeq]
    ThrowsQ,    // kids = [bodySeq]
    Ensure,     // kids = [bodySeq]
    Do,         // kids = [Seq or expr]
    Return,     // kids = [expr]
    While,      // kids = [condSeq, bodySeq]
    Until,      // kids = [bodySeq, condSeq]
    Loop,       // kids = [collection, Params, bodySeq]
    Higher,     // name = map/select/...; kids = [collection, Params, bodySeq]
    Range,      // kids = [from, to]
    Dictionary, // kids = [bodySeq of Define nodes] (`dictionary [...]`, classes)
    Passthrough,// con = preserved source token (the original's only seam)
    Break,
    Continue,
    Constructor,// kids[0] = Params
    Is,         // kids = [base, Dictionary]
};

enum class CKind : std::uint8_t {
    Null, Int, BigInt, Float, Str, Char, Bool,
    Word, Literal, Symbol, SymbolLiteral, Type, Label,
    Attribute, AttributeLabel, PathLiteral, Block, Inline,
    Quantity, Unit, Version, Color, Regex,
};

struct Const {
    CKind kind = CKind::Null;
    std::int64_t i = 0;          // Int
    double f = 0;                // Float
    bool b = false;              // Bool
    std::string s;               // Str/Char/BigInt text/token spellings
    std::vector<Const> items;    // Block/Inline elements, PathLiteral segments
    bool operator==(const Const& o) const {
        return kind == o.kind && i == o.i && f == o.f && b == o.b && s == o.s && items == o.items;
    }
};

struct Node {
    Op op = Op::Const;
    std::uint32_t name = 0xFFFFFFFFu;  // string pool index or kNone
    std::uint32_t con = 0xFFFFFFFFu;   // constant pool index or kNone
    std::uint32_t aux = 0;             // Block: 1 = raw [...] data, 0 = @[...]
    std::vector<std::uint32_t> kids;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> attrs;  // (name, kid)
    std::vector<std::uint32_t> names;  // Params: parameter name indices
};

inline constexpr std::uint32_t kNone = 0xFFFFFFFFu;

struct Program {
    std::vector<std::string> strings;
    std::unordered_map<std::string, std::uint32_t> stringIndex;
    std::vector<Const> consts;
    std::unordered_map<std::string, std::uint32_t> constIndex;
    std::vector<Node> nodes;
    std::uint32_t root = kNone;
    std::string sourcePath = "<script>";

    std::uint32_t intern(const std::string& s) {
        auto it = stringIndex.find(s);
        if (it != stringIndex.end()) return it->second;
        auto id = static_cast<std::uint32_t>(strings.size());
        strings.push_back(s);
        stringIndex.emplace(strings.back(), id);
        return id;
    }
    const std::string& str(std::uint32_t id) const { return strings.at(id); }

    static std::string constKey(const Const& c) {
        std::string k;
        k += static_cast<char>('a' + static_cast<int>(c.kind));
        k += '|' + std::to_string(c.i) + '|' + std::to_string(c.f) + '|' + (c.b ? "1" : "0");
        k += '|' + std::to_string(c.s.size()) + ':' + c.s;
        for (const auto& it : c.items) k += '{' + constKey(it) + '}';
        return k;
    }
    std::uint32_t addConst(Const c) {
        std::string key = constKey(c);
        auto it = constIndex.find(key);
        if (it != constIndex.end()) return it->second;
        auto id = static_cast<std::uint32_t>(consts.size());
        consts.push_back(std::move(c));
        constIndex.emplace(std::move(key), id);
        return id;
    }
    std::uint32_t addNode(Node n) {
        auto id = static_cast<std::uint32_t>(nodes.size());
        nodes.push_back(std::move(n));
        return id;
    }
    std::uint32_t make(Op op, std::vector<std::uint32_t> kids = {}) {
        Node n; n.op = op; n.kids = std::move(kids);
        return addNode(std::move(n));
    }
    std::uint32_t makeConst(Const c) {
        Node n; n.op = Op::Const; n.con = addConst(std::move(c));
        return addNode(std::move(n));
    }
    std::uint32_t makeNamed(Op op, const std::string& name, std::vector<std::uint32_t> kids = {}) {
        Node n; n.op = op; n.name = intern(name); n.kids = std::move(kids);
        return addNode(std::move(n));
    }
};

// ---- binary codec (AIR1) -------------------------------------------------
void writeProgram(const Program& p, std::vector<std::uint8_t>& out);
bool readProgram(const std::uint8_t* data, std::size_t size, Program& p, std::string& err);
const char* opName(Op op);

}  // namespace air
