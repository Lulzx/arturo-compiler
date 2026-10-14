// emit.hpp -- generic call tree -> compact IR.
//
// Mirrors the original kernel's emit column (kernel.art emitNode/emitCall and
// the defRule emit functions): one IR node per construct, lazy constructs get
// dedicated ops, everything else is a `call` whose callee decides at runtime
// between a closure and a donated primitive.
#pragma once

#include <charconv>
#include <string>
#include <unordered_set>
#include <vector>

#include "ir.hpp"
#include "lexer.hpp"
#include "parser.hpp"

namespace artc {

class Emitter {
  public:
    Emitter(Grouper& grouper, air::Program& prog) : g_(grouper), p_(prog) {}

    std::uint32_t emitProgram(const std::vector<GPtr>& roots) {
        auto seq = emitSeq(roots);
        p_.root = seq;
        return seq;
    }
    std::uint32_t emitSeq(const std::vector<GPtr>& roots) {
        std::vector<std::uint32_t> kids;
        for (const auto& r : roots) kids.push_back(emit(r));
        return p_.make(air::Op::Seq, std::move(kids));
    }

    std::uint32_t emit(const GPtr& g) {
        switch (g->kind) {
            case GNode::Kind::Define: {
                std::uint32_t expr = g->args.empty() ? constNull() : emit(g->args[0]);
                return p_.makeNamed(air::Op::Define, g_.emitName(g->name), {expr});
            }
            case GNode::Kind::BlockRaw: {
                if (g->evaluated) {
                    auto sub = g_.parseSeq(g->token.kids);
                    std::vector<std::uint32_t> kids;
                    for (const auto& r : sub.roots) kids.push_back(emit(r));
                    auto id = p_.make(air::Op::Block, std::move(kids));
                    p_.nodes[id].aux = 0;
                    return id;
                }
                std::vector<std::uint32_t> kids;
                for (const auto& t : g->token.kids) kids.push_back(dataNode(t));
                auto id = p_.make(air::Op::Block, std::move(kids));
                p_.nodes[id].aux = 1;
                return id;
            }
            case GNode::Kind::Inline: {
                auto sub = g_.parseSeq(g->token.kids);
                return p_.make(air::Op::Do, {emitSeq(sub.roots)});
            }
            case GNode::Kind::Body:
                return p_.make(air::Op::Do, {emitSeq(g->args)});
            case GNode::Kind::DictLit: {
                std::vector<std::uint32_t> kids;
                kids.push_back(p_.makeNamed(air::Op::Load, "makeDict"));
                for (const auto& kv : g->pairs) {
                    air::Const kc; kc.kind = air::CKind::Str; kc.s = kv.first;
                    kids.push_back(p_.makeConst(kc));
                    kids.push_back(emit(kv.second));
                }
                return p_.make(air::Op::Call, std::move(kids));
            }
            case GNode::Kind::Call: return emitCall(g);
            case GNode::Kind::Leaf: return emitLeaf(g);
        }
        return constNull();
    }

  private:
    Grouper& g_;
    air::Program& p_;

    std::uint32_t constNull() {
        air::Const c; c.kind = air::CKind::Null;
        return p_.makeConst(c);
    }
    air::Const constOf(const Token& t) {
        air::Const c;
        switch (t.kind) {
            case TK::Int: {
                std::string digits;
                for (char ch : t.text) if (ch != '_') digits += ch;
                std::int64_t v = 0;
                auto res = std::from_chars(digits.data(), digits.data() + digits.size(), v);
                if (res.ec == std::errc()) { c.kind = air::CKind::Int; c.i = v; }
                else { c.kind = air::CKind::BigInt; c.s = digits; }
                return c;
            }
            case TK::Float: c.kind = air::CKind::Float; c.f = std::stod(t.text); return c;
            case TK::Str: c.kind = air::CKind::Str; c.s = t.text; return c;
            case TK::CharTok: c.kind = air::CKind::Char; c.s = t.text; return c;
            case TK::TypeTok: c.kind = air::CKind::Type; c.s = t.text; return c;
            case TK::Literal:
                if (t.literalPath) {
                    c.kind = air::CKind::PathLiteral;
                    for (const auto& seg : t.segs) {
                        air::Const s; s.kind = air::CKind::Str; s.s = seg.name;
                        c.items.push_back(s);
                    }
                } else { c.kind = air::CKind::Literal; c.s = t.text; }
                return c;
            case TK::Quantity: {
                c.kind = air::CKind::Quantity; c.s = t.unit;
                bool integral = t.text.find('.') == std::string::npos &&
                                t.text.find('e') == std::string::npos && t.text.find('E') == std::string::npos;
                c.b = integral;
                if (integral) { try { c.i = std::stoll(t.text); c.f = static_cast<double>(c.i); } catch (...) { c.f = 0; } }
                else { try { c.f = std::stod(t.text); } catch (...) { c.f = 0; } }
                return c;
            }
            case TK::UnitTok: c.kind = air::CKind::Unit; c.s = t.text; return c;
            case TK::VersionTok: c.kind = air::CKind::Version; c.s = t.text; return c;
            case TK::ColorTok: c.kind = air::CKind::Color; c.s = t.text; return c;
            case TK::RegexTok: c.kind = air::CKind::Regex; c.s = t.text; return c;
            case TK::SymbolLit: c.kind = air::CKind::SymbolLiteral; c.s = t.text; return c;
            case TK::Symbol: c.kind = air::CKind::Symbol; c.s = t.text; return c;
            case TK::Attr: c.kind = air::CKind::Attribute; c.s = t.text; return c;
            case TK::AttrLabel: c.kind = air::CKind::AttributeLabel; c.s = t.text; return c;
            case TK::Label: case TK::LabelStr: c.kind = air::CKind::Label; c.s = t.text; return c;
            case TK::Word:
                if (t.text == "true") { c.kind = air::CKind::Bool; c.b = true; }
                else if (t.text == "false") { c.kind = air::CKind::Bool; c.b = false; }
                else if (t.text == "null") { c.kind = air::CKind::Null; }
                else { c.kind = air::CKind::Word; c.s = t.text; }
                return c;
            case TK::Block:
                c.kind = air::CKind::Block;
                for (const auto& k : t.kids) c.items.push_back(constOf(k));
                return c;
            case TK::Inline:
                c.kind = air::CKind::Inline;
                for (const auto& k : t.kids) c.items.push_back(constOf(k));
                return c;
            case TK::Path: case TK::PathLabel:
                c.kind = air::CKind::Block;
                for (const auto& seg : t.segs) {
                    air::Const s; s.kind = air::CKind::Word; s.s = seg.name;
                    c.items.push_back(s);
                }
                return c;
            case TK::End: return c;
        }
        return c;
    }
    std::uint32_t dataNode(const Token& t) {
        // a path inside block data stays an unresolved path-shaped block,
        // matching the original's raw-items block representation closely
        // enough for value positions; evaluated paths never reach here.
        return p_.makeConst(constOf(t));
    }
    std::uint32_t emitLeaf(const GPtr& g) {
        if (!g->name.empty()) {
            if (g->name == "true" || g->name == "false" || g->name == "null") {
                Token t; t.kind = TK::Word; t.text = g->name;
                return p_.makeConst(constOf(t));
            }
            return g->intrinsic ? p_.makeNamed(air::Op::Intrinsic, g->name)
                                : p_.makeNamed(air::Op::Load, g->name);
        }
        return p_.makeConst(constOf(g->token));
    }
    // a body argument: [block] / -> body / inline -> a Seq node
    std::uint32_t bodyOf(const GPtr& g) {
        if (g->kind == GNode::Kind::BlockRaw && !g->evaluated) {
            auto sub = g_.parseSeq(g->token.kids);
            return emitSeq(sub.roots);
        }
        if (g->kind == GNode::Kind::Body) return emitSeq(g->args);
        if (g->kind == GNode::Kind::Inline) {
            auto sub = g_.parseSeq(g->token.kids);
            return emitSeq(sub.roots);
        }
        // a non-block body (e.g. `map xs [x] stored`) stays the expression
        // itself: the runtime's higher/loop handling distinguishes a stored
        // block value from an inline __seq, and wrapping would hide it
        return emit(g);
    }
    std::uint32_t paramsOf(const GPtr& g) {
        air::Node n; n.op = air::Op::Params;
        if (g->kind == GNode::Kind::BlockRaw) {
            for (const auto& t : g->token.kids)
                if (t.kind == TK::Word) n.names.push_back(p_.intern(t.text));
        } else if (g->kind == GNode::Kind::Leaf && !g->name.empty()) {
            n.names.push_back(p_.intern(g->name));
        } else if (g->kind == GNode::Kind::Leaf &&
                   (g->token.kind == TK::Word || g->token.kind == TK::Literal ||
                    g->token.kind == TK::SymbolLit)) {
            // a single quoted parameter: loop xs 'x [...]
            n.names.push_back(p_.intern(g->token.text));
        }
        return p_.addNode(std::move(n));
    }
    void attachAttrs(std::uint32_t callId, const GPtr& g) {
        for (const auto& a : g->attrs) {
            std::uint32_t kid;
            if (a.second->kind == GNode::Kind::Leaf && a.second->name == "true" && a.second->intrinsic) {
                air::Const c; c.kind = air::CKind::Bool; c.b = true;
                kid = p_.makeConst(c);
            } else kid = emit(a.second);
            p_.nodes[callId].attrs.emplace_back(p_.intern(a.first), kid);
        }
    }

    std::uint32_t emitCall(const GPtr& g) {
        const std::string name = Grouper::calleeName(g->fn);
        const bool intrinsicCallee = g->fn && g->fn->kind == GNode::Kind::Leaf && g->fn->intrinsic;
        if (!name.empty() && intrinsicCallee) {
            if (g_.isUnsupported(name))
                throw CompileError("unsupported standalone capability: " + name);
            auto& a = g->args;
            auto need = [&](std::size_t n) {
                if (a.size() < n) throw CompileError("compiler: '" + name + "' called with too few arguments");
            };
            if (name == "function" || name == "method") {
                need(2);
                std::vector<std::uint32_t> kids{paramsOf(a[0])};
                auto body = bodyOf(a[1]);
                for (auto k : p_.nodes[body].kids) kids.push_back(k);
                auto id = p_.make(name == "function" ? air::Op::Function : air::Op::Method, std::move(kids));
                attachAttrs(id, g);
                return id;
            }
            if (name == "constructor") { need(1); return p_.make(air::Op::Constructor, {paramsOf(a[0])}); }
            if (name == "if") { need(2); auto id = p_.make(air::Op::If, {emit(a[0]), bodyOf(a[1])}); attachAttrs(id, g); return id; }
            if (name == "unless") { need(2); return p_.make(air::Op::Unless, {emit(a[0]), bodyOf(a[1])}); }
            if (name == "switch") { need(3); return p_.make(air::Op::Switch, {emit(a[0]), bodyOf(a[1]), bodyOf(a[2])}); }
            if (name == "when") {
                need(1);
                std::vector<std::uint32_t> kids;
                if (a[0]->kind == GNode::Kind::BlockRaw) {
                    auto sub = g_.parseSeq(a[0]->token.kids);
                    for (std::size_t j = 0; j + 1 < sub.roots.size(); j += 2) {
                        kids.push_back(emit(sub.roots[j]));
                        kids.push_back(bodyOf(sub.roots[j + 1]));
                    }
                }
                return p_.make(air::Op::When, std::move(kids));
            }
            if (name == "using") { need(2); return p_.make(air::Op::Using, {emit(a[0]), bodyOf(a[1])}); }
            if (name == "try") { need(1); return p_.make(air::Op::Try, {bodyOf(a[0])}); }
            if (name == "throws?") { need(1); return p_.make(air::Op::ThrowsQ, {bodyOf(a[0])}); }
            if (name == "ensure") { need(1); return p_.make(air::Op::Ensure, {bodyOf(a[0])}); }
            if (name == "do") {
                need(1);
                if (a[0]->kind == GNode::Kind::BlockRaw || a[0]->kind == GNode::Kind::Body ||
                    a[0]->kind == GNode::Kind::Inline)
                    return p_.make(air::Op::Do, {bodyOf(a[0])});
                return p_.make(air::Op::Do, {emit(a[0])});
            }
            if (name == "return") {
                if (a.empty()) return p_.make(air::Op::Return, {constNull()});
                return p_.make(air::Op::Return, {emit(a[0])});
            }
            if (name == "while") { need(2); return p_.make(air::Op::While, {bodyOf(a[0]), bodyOf(a[1])}); }
            if (name == "until") { need(2); return p_.make(air::Op::Until, {bodyOf(a[0]), bodyOf(a[1])}); }
            if (name == "loop") {
                need(3);
                if ((a[2]->kind == GNode::Kind::BlockRaw && !a[2]->evaluated) ||
                    a[2]->kind == GNode::Kind::Body) {
                    auto id = p_.make(air::Op::Loop, {emit(a[0]), paramsOf(a[1]), bodyOf(a[2])});
                    attachAttrs(id, g);
                    return id;
                }
                // stored action: fall through to the generic call below
            }
            static const std::unordered_set<std::string> kHigher = {
                "map", "select", "filter", "fold", "every?", "some?", "chunk", "cluster",
                "gather", "arrange", "enumerate", "maximum", "minimum"};
            if (kHigher.count(name)) {
                need(3);
                // the `higher` op is only for a literal block action with a
                // literal parameter block (kernel emitHigher); anything else
                // is a plain intrinsic call carrying block values. A `->`
                // body counts as a literal action, as in the host.
                bool literalAction = (a[2]->kind == GNode::Kind::BlockRaw && !a[2]->evaluated) ||
                                     a[2]->kind == GNode::Kind::Body;
                bool literalParams = a[1]->kind == GNode::Kind::BlockRaw ||
                                     (a[1]->kind == GNode::Kind::Leaf && a[1]->token.kind == TK::Literal);
                if (literalAction && literalParams) {
                    auto id = p_.makeNamed(air::Op::Higher, name, {emit(a[0]), paramsOf(a[1]), bodyOf(a[2])});
                    attachAttrs(id, g);
                    return id;
                }
            }
            if (name == "dictionary") { need(1); return p_.make(air::Op::Dictionary, {bodyOf(a[0])}); }
            if (name == "is") {
                need(2);
                auto dict = p_.make(air::Op::Dictionary, {bodyOf(a[1])});
                return p_.make(air::Op::Is, {emit(a[0]), dict});
            }
            if (name == "let") {
                need(2);
                std::string var = Grouper::calleeName(a[0]);
                if (var.empty() && a[0]->kind == GNode::Kind::Leaf) var = a[0]->token.text;
                return p_.makeNamed(air::Op::Let, var, {emit(a[1])});
            }
            if (name == "break") return p_.make(air::Op::Break);
            if (name == "continue") return p_.make(air::Op::Continue);
            if (name == "range") { need(2); auto id = p_.make(air::Op::Range, {emit(a[0]), emit(a[1])}); attachAttrs(id, g); return id; }
            if (name == "sortable") {
                need(1);
                std::string field = Grouper::calleeName(a[0]);
                if (field.empty() && a[0]->kind == GNode::Kind::Leaf) field = a[0]->token.text;
                if (field.empty()) throw CompileError("sortable: expected a field name");
                air::Node params; params.op = air::Op::Params;
                params.names.push_back(p_.intern("that"));
                auto paramsId = p_.addNode(std::move(params));
                auto fieldKey = [&]() {
                    air::Const c; c.kind = air::CKind::Literal; c.s = field;
                    auto id = p_.makeConst(c);
                    p_.nodes[id].op = air::Op::Passthrough;
                    return id;
                };
                auto get = [&](const char* who) {
                    std::vector<std::uint32_t> kids{
                        p_.makeNamed(air::Op::Intrinsic, "pathGet"),
                        p_.makeNamed(air::Op::Load, who), fieldKey()};
                    return p_.make(air::Op::Call, std::move(kids));
                };
                std::vector<std::uint32_t> cmpKids{
                    p_.makeNamed(air::Op::Intrinsic, "compare"), get("this"), get("that")};
                auto cmp = p_.make(air::Op::Call, std::move(cmpKids));
                return p_.make(air::Op::Method, {paramsId, cmp});
            }
            if (name == "define") {
                need(2);
                std::string typeName;
                if (a[0]->kind == GNode::Kind::Leaf) typeName = a[0]->token.text;
                if (typeName.empty()) typeName = Grouper::calleeName(a[0]);
                if (typeName.empty()) throw CompileError("define: expected a :type name");
                std::uint32_t expr;
                if (a[1]->kind == GNode::Kind::BlockRaw && !a[1]->evaluated)
                    expr = p_.make(air::Op::Dictionary, {bodyOf(a[1])});
                else expr = emit(a[1]);
                return p_.makeNamed(air::Op::Define, "__class_" + typeName, {expr});
            }
        }
        // generic call; a mutating primitive's first argument is a reference
        std::vector<std::uint32_t> kids;
        kids.push_back(emit(g->fn));
        for (std::size_t j = 0; j < g->args.size(); ++j) {
            if (j == 0 && !name.empty() && intrinsicCallee && g_.isMutating(name)) {
                auto& arg = g->args[0];
                if (arg->kind == GNode::Kind::Leaf && arg->token.kind == TK::Literal && !arg->token.literalPath) {
                    air::Const pc; pc.kind = air::CKind::PathLiteral;
                    air::Const seg; seg.kind = air::CKind::Str; seg.s = arg->token.text;
                    pc.items.push_back(seg);
                    auto id = p_.makeConst(pc);
                    p_.nodes[id].op = air::Op::Passthrough;
                    kids.push_back(id);
                    continue;
                }
            }
            kids.push_back(emit(g->args[j]));
        }
        auto id = p_.make(air::Op::Call, std::move(kids));
        attachAttrs(id, g);
        return id;
    }
};

}  // namespace artc
