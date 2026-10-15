// verify.hpp -- structural IR verifier, run after emission and after the
// optimizer. It checks the invariants every backend relies on -- pool
// indices in range, per-op child shapes, attributes referencing real nodes
// -- so a bad pass fails at compile time with a named node instead of
// producing a broken native binary. (Effect ordering is preserved by
// construction: the optimizer never reorders, removes, or duplicates a node
// that is not a provably pure constant; see optimizer.hpp.)
#pragma once

#include <cstdint>
#include <string>

#include "ir.hpp"

namespace artc {

inline bool verifyProgram(const air::Program& p, std::string& err) {
    if (p.root == air::kNone || p.root >= p.nodes.size()) {
        err = "root node out of range";
        return false;
    }
    for (std::size_t i = 0; i < p.nodes.size(); ++i) {
        const auto& n = p.nodes[i];
        auto bad = [&](const std::string& what) {
            err = "node " + std::to_string(i) + " (" + air::opName(n.op) + "): " + what;
            return false;
        };
        if (n.name != air::kNone && n.name >= p.strings.size()) return bad("name index out of range");
        if (n.con != air::kNone && n.con >= p.consts.size()) return bad("constant index out of range");
        for (auto k : n.kids) if (k >= p.nodes.size()) return bad("child index out of range");
        for (auto& a : n.attrs) {
            if (a.first >= p.strings.size()) return bad("attribute name out of range");
            if (a.second >= p.nodes.size()) return bad("attribute value out of range");
        }
        for (auto nm : n.names) if (nm >= p.strings.size()) return bad("parameter name out of range");
        using O = air::Op;
        switch (n.op) {
            case O::Const: case O::Passthrough:
                if (n.con == air::kNone) return bad("missing constant");
                break;
            case O::Load: case O::Intrinsic:
                if (n.name == air::kNone) return bad("missing name");
                break;
            case O::Define: case O::Let:
                if (n.name == air::kNone || n.kids.size() != 1) return bad("needs a name and one expression");
                break;
            case O::Call:
                if (n.kids.empty()) return bad("call needs a callee");
                break;
            case O::Function: case O::Method:
                if (n.kids.empty() || p.nodes[n.kids[0]].op != O::Params)
                    return bad("function needs a parameter list first");
                break;
            case O::If:
                if (n.kids.size() < 2 || n.kids.size() > 3) return bad("if needs condition and body");
                break;
            case O::While: case O::Until: case O::Switch:
                if (n.kids.size() != 2 && n.op != O::Switch) return bad("needs two children");
                if (n.op == O::Switch && n.kids.size() != 3) return bad("switch needs three children");
                break;
            case O::Loop: case O::Higher:
                if (n.kids.size() != 3 || p.nodes[n.kids[1]].op != O::Params)
                    return bad("needs collection, parameters, and body");
                break;
            case O::Constructor:
                if (n.kids.size() != 1 || p.nodes[n.kids[0]].op != O::Params)
                    return bad("constructor needs a parameter list");
                break;
            case O::Range: case O::Is:
                if (n.kids.size() != 2) return bad("needs two children");
                break;
            case O::Return:
                if (n.kids.size() > 1) return bad("return takes at most one value");
                break;
            default: break;
        }
    }
    return true;
}

}  // namespace artc
