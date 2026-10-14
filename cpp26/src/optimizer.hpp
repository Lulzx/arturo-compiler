// optimizer.hpp -- IR->IR optimization passes.
//
// Every pass preserves the contract invariant eval(emit(n)) == eval(n) by
// construction: it only rewrites a node when the replacement is provably the
// same value under the runtime's own semantics, and it never moves, removes,
// or executes a side effect at compile time. The pure-intrinsic allowlist is
// the original's own judgement (semantics.art PURE_INTRINSICS, generated into
// tables.inc); a call through a *name* is never folded, because that name may
// be bound to a user closure when the call runs.
#pragma once

#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "ir.hpp"

namespace artc {

struct OptStats {
    int folded = 0;
    int branchesFolded = 0;
    int deadRemoved = 0;
    int seqsFlattened = 0;
    std::size_t nodesBefore = 0, nodesAfter = 0;
};

class Optimizer {
  public:
    using PureFn = std::function<bool(const std::string&)>;
    explicit Optimizer(PureFn pure) : pure_(std::move(pure)) {}

    OptStats run(air::Program& p) {
        OptStats st;
        st.nodesBefore = p.nodes.size();
        for (int iter = 0; iter < 8; ++iter) {
            int before = st.folded + st.branchesFolded + st.deadRemoved + st.seqsFlattened;
            foldAll(p, p.root, st);
            foldBranches(p, p.root, st);
            cleanSeq(p, p.root, st);
            propagateSeq(p, p.root, st);
            int after = st.folded + st.branchesFolded + st.deadRemoved + st.seqsFlattened;
            if (after == before) break;
        }
        compact(p);
        st.nodesAfter = p.nodes.size();
        return st;
    }

  private:
    PureFn pure_;

    static bool isConst(const air::Program& p, std::uint32_t id) {
        return p.nodes[id].op == air::Op::Const;
    }
    static const air::Const& con(const air::Program& p, std::uint32_t id) {
        return p.consts[p.nodes[id].con];
    }
    static bool numeric(const air::Const& c, double& d) {
        if (c.kind == air::CKind::Int) { d = static_cast<double>(c.i); return true; }
        if (c.kind == air::CKind::Float) { d = c.f; return true; }
        return false;
    }
    static air::Const makeInt(std::int64_t v) { air::Const c; c.kind = air::CKind::Int; c.i = v; return c; }
    static air::Const makeFloat(double v) { air::Const c; c.kind = air::CKind::Float; c.f = v; return c; }
    static air::Const makeBool(bool v) { air::Const c; c.kind = air::CKind::Bool; c.b = v; return c; }
    static std::size_t utf8Len(const std::string& s) {
        std::size_t n = 0;
        for (unsigned char ch : s) if ((ch & 0xC0) != 0x80) ++n;
        return n;
    }
    static std::optional<int> cmpConst(const air::Const& a, const air::Const& b) {
        double da, db;
        if (numeric(a, da) && numeric(b, db)) return da < db ? -1 : da > db ? 1 : 0;
        if (a.kind == air::CKind::Str && b.kind == air::CKind::Str)
            return a.s < b.s ? -1 : a.s > b.s ? 1 : 0;
        if (a.kind == air::CKind::Bool && b.kind == air::CKind::Bool)
            return static_cast<int>(a.b) - static_cast<int>(b.b);
        return std::nullopt;
    }
    static bool equalConst(const air::Const& a, const air::Const& b) {
        double da, db;
        if (numeric(a, da) && numeric(b, db)) return da == db;
        if (a.kind != b.kind) return a.kind == air::CKind::Null && b.kind == air::CKind::Null;
        switch (a.kind) {
            case air::CKind::Null: return true;
            case air::CKind::Str: case air::CKind::Char: case air::CKind::Word:
            case air::CKind::Literal: case air::CKind::Symbol: case air::CKind::SymbolLiteral:
            case air::CKind::Type: case air::CKind::Label:
            case air::CKind::Color: case air::CKind::Regex: case air::CKind::Unit:
            case air::CKind::BigInt: case air::CKind::Attribute: case air::CKind::AttributeLabel:
                return a.s == b.s;
            case air::CKind::Version: {
                // build metadata (+...) does not affect version equality
                auto core = [](const std::string& s) { return s.substr(0, s.find('+')); };
                return core(a.s) == core(b.s);
            }
            case air::CKind::Quantity:
                return a.s == b.s && a.i == b.i && a.f == b.f;
            case air::CKind::Bool: return a.b == b.b;
            case air::CKind::Block: case air::CKind::Inline: case air::CKind::PathLiteral:
                return a.items == b.items;
            default: return false;
        }
    }

    // Evaluate a pure intrinsic call whose arguments are all constants.
    // Returns nullopt whenever the runtime could do anything we do not model
    // exactly (errors, big-int overflow, exotic kinds): not folding is always
    // correct, mis-folding never is.
    std::optional<air::Const> evalPure(const std::string& name,
                                       const std::vector<air::Const>& a) {
        auto ints2 = [&]() -> std::optional<std::pair<std::int64_t, std::int64_t>> {
            if (a.size() == 2 && a[0].kind == air::CKind::Int && a[1].kind == air::CKind::Int)
                return std::pair{a[0].i, a[1].i};
            return std::nullopt;
        };
        if (name == "add" || name == "sub" || name == "mul" || name == "div" ||
            name == "fdiv" || name == "mod" || name == "pow") {
            if (auto pr = ints2()) {
                __int128 x = pr->first, y = pr->second, r = 0;
                if (name == "add") r = x + y;
                else if (name == "sub") r = x - y;
                else if (name == "mul") r = x * y;
                else if (name == "div") { if (y == 0) return std::nullopt; r = x / y; }
                else if (name == "mod") { if (y == 0) return std::nullopt; r = x % y; }
                else if (name == "pow") {
                    if (y < 0 || y > 62) return std::nullopt;
                    r = 1; for (std::int64_t i = 0; i < y; ++i) r *= x;
                } else if (name == "fdiv") {
                    if (y == 0) return std::nullopt;
                    return makeFloat(static_cast<double>(x) / static_cast<double>(y));
                } else return std::nullopt;
                if (r > INT64_MAX || r < INT64_MIN) return std::nullopt;
                return makeInt(static_cast<std::int64_t>(r));
            }
            double x, y;
            if (a.size() == 2 && numeric(a[0], x) && numeric(a[1], y)) {
                if ((name == "div" || name == "fdiv" || name == "mod") && y == 0) return std::nullopt;
                double r = name == "add" ? x + y : name == "sub" ? x - y : name == "mul" ? x * y
                           : name == "pow" ? std::pow(x, y) : name == "mod" ? std::fmod(x, y) : x / y;
                return makeFloat(r);
            }
            if (name == "add" && a.size() == 2 && a[0].kind == air::CKind::Str &&
                a[1].kind == air::CKind::Str) {
                air::Const c; c.kind = air::CKind::Str; c.s = a[0].s + a[1].s; return c;
            }
            return std::nullopt;
        }
        if (name == "append" && a.size() == 2) {
            if (a[0].kind == air::CKind::Str && a[1].kind == air::CKind::Str) {
                air::Const c; c.kind = air::CKind::Str; c.s = a[0].s + a[1].s; return c;
            }
            if (a[0].kind == air::CKind::Str && a[1].kind == air::CKind::Int) {
                // documented host-defect exception: string ++ integer renders
                air::Const c; c.kind = air::CKind::Str; c.s = a[0].s + std::to_string(a[1].i); return c;
            }
            return std::nullopt;
        }
        if (name == "equal?" || name == "notEqual?" || name == "less?" || name == "greater?" ||
            name == "lessOrEqual?" || name == "greaterOrEqual?") {
            if (a.size() != 2) return std::nullopt;
            // quantity, rational, unit, and color equality is
            // conversion/canonicalization-based in the runtime (converted
            // amounts, unit synonyms, color values): never fold it
            if (a[0].kind == air::CKind::Quantity || a[1].kind == air::CKind::Quantity ||
                a[0].kind == air::CKind::Rational || a[1].kind == air::CKind::Rational ||
                a[0].kind == air::CKind::Unit || a[1].kind == air::CKind::Unit ||
                a[0].kind == air::CKind::Color || a[1].kind == air::CKind::Color)
                return std::nullopt;
            if (name == "equal?") return makeBool(equalConst(a[0], a[1]));
            if (name == "notEqual?") return makeBool(!equalConst(a[0], a[1]));
            auto c = cmpConst(a[0], a[1]);
            if (!c) return std::nullopt;
            return makeBool(name == "less?" ? *c < 0 : name == "greater?" ? *c > 0
                            : name == "lessOrEqual?" ? *c <= 0 : *c >= 0);
        }
        if (name == "and?" || name == "or?") {
            if (a.size() == 2 && a[0].kind == air::CKind::Bool && a[1].kind == air::CKind::Bool)
                return makeBool(name == "and?" ? (a[0].b && a[1].b) : (a[0].b || a[1].b));
            return std::nullopt;
        }
        if (name == "not?" && a.size() == 1) {
            bool truthy = !(a[0].kind == air::CKind::Null || (a[0].kind == air::CKind::Bool && !a[0].b));
            return makeBool(!truthy);
        }
        if (name == "null?" && a.size() == 1) return makeBool(a[0].kind == air::CKind::Null);
        if (name == "inc" || name == "dec") {
            if (a.size() == 1 && a[0].kind == air::CKind::Int) {
                __int128 r = static_cast<__int128>(a[0].i) + (name == "inc" ? 1 : -1);
                if (r > INT64_MAX || r < INT64_MIN) return std::nullopt;
                return makeInt(static_cast<std::int64_t>(r));
            }
            double d; if (a.size() == 1 && numeric(a[0], d)) return makeFloat(d + (name == "inc" ? 1 : -1));
            return std::nullopt;
        }
        if (name == "size" && a.size() == 1) {
            if (a[0].kind == air::CKind::Str) return makeInt(static_cast<std::int64_t>(utf8Len(a[0].s)));
            if (a[0].kind == air::CKind::Block) return makeInt(static_cast<std::int64_t>(a[0].items.size()));
            return std::nullopt;
        }
        if (name == "type" && a.size() == 1) {
            const char* t = a[0].kind == air::CKind::Int ? "integer" : a[0].kind == air::CKind::Float ? "floating"
                : a[0].kind == air::CKind::Str ? "string" : a[0].kind == air::CKind::Char ? "char"
                : a[0].kind == air::CKind::Bool ? "logical" : a[0].kind == air::CKind::Block ? "block"
                : a[0].kind == air::CKind::Null ? "null" : nullptr;
            if (!t) return std::nullopt;
            air::Const c; c.kind = air::CKind::Type; c.s = t; return c;
        }
        if ((name == "sqrt" || name == "ln" || name == "log" || name == "sin" || name == "cos" ||
             name == "tan" || name == "exp") && a.size() == 1) {
            double d;
            if (!numeric(a[0], d)) return std::nullopt;
            if (name == "sqrt" && d < 0) return std::nullopt;
            if ((name == "ln" || name == "log") && d <= 0) return std::nullopt;
            double r = name == "sqrt" ? std::sqrt(d) : name == "ln" ? std::log(d) : name == "log" ? std::log10(d)
                       : name == "sin" ? std::sin(d) : name == "cos" ? std::cos(d) : name == "tan" ? std::tan(d)
                       : std::exp(d);
            return makeFloat(r);
        }
        if (name == "to" && a.size() == 2 && a[0].kind == air::CKind::Type) {
            const std::string& t = a[0].s;
            if (t == "string" && a[1].kind == air::CKind::Int) {
                air::Const c; c.kind = air::CKind::Str; c.s = std::to_string(a[1].i); return c;
            }
            if (t == "integer" && a[1].kind == air::CKind::Str) {
                try { return makeInt(std::stoll(a[1].s)); } catch (...) { return std::nullopt; }
            }
            if (t == "integer" && a[1].kind == air::CKind::Float) return makeInt(static_cast<std::int64_t>(a[1].f));
            if (t == "floating" && a[1].kind == air::CKind::Int) return makeFloat(static_cast<double>(a[1].i));
            return std::nullopt;
        }
        return std::nullopt;
    }

    void foldAll(air::Program& p, std::uint32_t id, OptStats& st) {
        auto& node = p.nodes[id];
        std::vector<std::uint32_t> kids = node.kids;
        for (auto k : kids) foldAll(p, k, st);
        for (auto& a : node.attrs) foldAll(p, a.second, st);
        if (node.op != air::Op::Call || node.kids.empty()) return;
        const auto& fn = p.nodes[node.kids[0]];
        if (fn.op != air::Op::Intrinsic || fn.name == air::kNone) return;
        const std::string name = p.str(fn.name);
        if (!pure_(name)) return;
        std::vector<air::Const> argv;
        for (std::size_t i = 1; i < node.kids.size(); ++i) {
            if (!isConst(p, node.kids[i])) return;
            argv.push_back(con(p, node.kids[i]));
        }
        auto v = evalPure(name, argv);
        if (!v) return;
        auto cid = p.addConst(*v);
        node.op = air::Op::Const; node.con = cid; node.name = air::kNone;
        node.kids.clear(); node.attrs.clear();
        ++st.folded;
    }
    void foldBranches(air::Program& p, std::uint32_t id, OptStats& st) {
        auto& node = p.nodes[id];
        std::vector<std::uint32_t> kids = node.kids;
        for (auto k : kids) foldBranches(p, k, st);
        if (node.op == air::Op::If && node.kids.size() >= 2 && isConst(p, node.kids[0])) {
            const auto& c = con(p, node.kids[0]);
            bool truthy = !(c.kind == air::CKind::Null || (c.kind == air::CKind::Bool && !c.b));
            std::uint32_t branch = truthy ? node.kids[1]
                                          : (node.kids.size() > 2 ? node.kids[2] : air::kNone);
            if (branch != air::kNone) {
                p.nodes[id] = p.nodes[branch];
                ++st.branchesFolded;
            }
        }
        if (node.op == air::Op::Unless && node.kids.size() >= 2 && isConst(p, node.kids[0])) {
            const auto& c = con(p, node.kids[0]);
            bool truthy = !(c.kind == air::CKind::Null || (c.kind == air::CKind::Bool && !c.b));
            if (truthy) {
                air::Const nc; nc.kind = air::CKind::Null;
                auto cid = p.addConst(nc);
                p.nodes[id] = air::Node{}; p.nodes[id].op = air::Op::Const; p.nodes[id].con = cid;
                ++st.branchesFolded;
            } else {
                p.nodes[id] = p.nodes[node.kids[1]];
                ++st.branchesFolded;
            }
        }
    }
    void cleanSeq(air::Program& p, std::uint32_t id, OptStats& st) {
        auto& node = p.nodes[id];
        std::vector<std::uint32_t> kids = node.kids;
        for (auto k : kids) cleanSeq(p, k, st);
        if (node.op != air::Op::Seq) return;
        std::vector<std::uint32_t> out;
        for (std::size_t i = 0; i < node.kids.size(); ++i) {
            auto k = node.kids[i];
            if (p.nodes[k].op == air::Op::Seq) {
                for (auto sub : p.nodes[k].kids) out.push_back(sub);
                ++st.seqsFlattened;
                continue;
            }
            bool last = i + 1 == node.kids.size();
            if (!last && p.nodes[k].op == air::Op::Const) { ++st.deadRemoved; continue; }
            out.push_back(k);
            if (p.nodes[k].op == air::Op::Return || p.nodes[k].op == air::Op::Break ||
                p.nodes[k].op == air::Op::Continue) {
                st.deadRemoved += static_cast<int>(node.kids.size() - i - 1);
                break;
            }
        }
        node.kids = std::move(out);
    }
    bool subtreeHasCallOrControl(air::Program& p, std::uint32_t id, int depth) {
        if (depth > 32) return true;
        const auto& n = p.nodes[id];
        switch (n.op) {
            case air::Op::Call: case air::Op::If: case air::Op::While: case air::Op::Until:
            case air::Op::Loop: case air::Op::Function: case air::Op::Method: case air::Op::Higher:
            case air::Op::Define: case air::Op::Let: case air::Op::Return:
                return true;
            default: break;
        }
        for (auto k : n.kids) if (subtreeHasCallOrControl(p, k, depth + 1)) return true;
        return false;
    }
    void substitute(air::Program& p, std::uint32_t id,
                    const std::unordered_map<std::string, std::uint32_t>& env) {
        auto& n = p.nodes[id];
        if (n.op == air::Op::Load && n.name != air::kNone) {
            auto it = env.find(p.str(n.name));
            if (it != env.end()) {
                n.op = air::Op::Const; n.con = it->second; n.name = air::kNone;
                return;
            }
        }
        std::vector<std::uint32_t> kids = n.kids;
        for (auto k : kids) substitute(p, k, env);
    }
    void propagateSeq(air::Program& p, std::uint32_t id, OptStats& st) {
        (void)st;
        auto& node = p.nodes[id];
        std::vector<std::uint32_t> kids = node.kids;
        for (auto k : kids) propagateSeq(p, k, st);
        if (node.op != air::Op::Seq) return;
        std::unordered_map<std::string, std::uint32_t> env;
        for (auto k : node.kids) {
            if (!env.empty() && !subtreeHasCallOrControl(p, k, 0)) substitute(p, k, env);
            const auto& n = p.nodes[k];
            if ((n.op == air::Op::Define || n.op == air::Op::Let) && n.name != air::kNone &&
                !n.kids.empty() && isConst(p, n.kids[0])) {
                env[p.str(n.name)] = p.nodes[n.kids[0]].con;
            } else if (n.op == air::Op::Call || n.op == air::Op::Loop || n.op == air::Op::While ||
                       n.op == air::Op::Function || n.op == air::Op::Method) {
                env.clear();
            } else if ((n.op == air::Op::Define || n.op == air::Op::Let) && n.name != air::kNone) {
                env.erase(p.str(n.name));
            }
        }
    }
    void compact(air::Program& p) {
        std::unordered_map<std::uint32_t, std::uint32_t> remap;
        std::vector<air::Node> kept;
        std::function<void(std::uint32_t)> mark = [&](std::uint32_t id) {
            if (remap.count(id)) return;
            auto nid = static_cast<std::uint32_t>(kept.size());
            remap[id] = nid;
            kept.push_back(p.nodes[id]);
            for (auto k : p.nodes[id].kids) mark(k);
            for (auto& a : p.nodes[id].attrs) mark(a.second);
        };
        mark(p.root);
        for (auto& n : kept) {
            for (auto& k : n.kids) k = remap[k];
            for (auto& a : n.attrs) a.second = remap[a.second];
        }
        p.root = remap[p.root];
        p.nodes = std::move(kept);
    }
};

}  // namespace artc
