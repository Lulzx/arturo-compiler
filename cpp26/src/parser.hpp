// parser.hpp -- grouping: lexed tokens -> generic call tree.
//
// Mirrors the original front.art `lowerWalk`, which mirrors the host's
// processBlock: left-to-right build, a word's declared arity opens a call
// frame, a frame pops once it has exactly `arity` arguments, and an infix
// symbol takes the preceding value as its first operand with a single
// precedence (right-associative, rightmost binds tightest).
#pragma once

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "lexer.hpp"
#include "tables.inc"

namespace artc {

struct GNode {
    enum class Kind { Leaf, Call, Define, BlockRaw, Inline, Body, DictLit } kind = Kind::Leaf;
    Token token;                       // Leaf / BlockRaw / Inline payload
    bool intrinsic = false;            // Leaf: primitive (vs load)
    bool evaluated = false;            // BlockRaw: @[...] (vs raw [...])
    std::string name;                  // Leaf / Define name
    std::shared_ptr<GNode> fn;         // Call callee
    std::vector<std::shared_ptr<GNode>> args;
    std::vector<std::pair<std::string, std::shared_ptr<GNode>>> attrs;
    std::vector<std::pair<std::string, std::shared_ptr<GNode>>> pairs;  // DictLit
};
using GPtr = std::shared_ptr<GNode>;

inline GPtr gLeaf(Token t) {
    auto g = std::make_shared<GNode>();
    g->kind = GNode::Kind::Leaf; g->token = std::move(t);
    return g;
}
inline GPtr gName(const std::string& name, bool intrinsic) {
    auto g = std::make_shared<GNode>();
    g->kind = GNode::Kind::Leaf; g->name = name; g->intrinsic = intrinsic;
    g->token.kind = TK::Word; g->token.text = name;
    return g;
}
inline GPtr gCall(GPtr fn, std::vector<GPtr> args = {}) {
    auto g = std::make_shared<GNode>();
    g->kind = GNode::Kind::Call; g->fn = std::move(fn); g->args = std::move(args);
    return g;
}

class Grouper {
  public:
    Grouper() {
        for (const auto& b : kBuiltins) {
            builtins_[b.name] = &b;
            arity_[b.name] = b.arity;
            if (b.alias[0]) {
                if (b.infix) infix_[b.alias] = b.name;
                else if (std::string(b.alias) != "$" && std::string(b.alias) != "#" &&
                         std::string(b.alias) != "@")
                    prefix_[b.alias] = b.name;
            }
            if (b.mutates && std::string(b.name) != "call" && std::string(b.name) != "info")
                mutates_.insert(b.name);
        }
        for (const char* n : kPureIntrinsics) pure_.insert(n);
        for (const char* n : kUnsupportedIntrinsics) unsupported_.insert(n);
        for (const char* n : kShadowable) shadowable_.insert(n);
        arity_["fold"] = 3;
        arity_["super"] = 0;
    }

    const BuiltinInfo* builtin(const std::string& n) const {
        auto it = builtins_.find(n);
        return it == builtins_.end() ? nullptr : it->second;
    }
    bool isMutating(const std::string& n) const { return mutates_.count(n) != 0; }
    bool isPure(const std::string& n) const { return pure_.count(n) != 0; }
    bool isUnsupported(const std::string& n) const { return unsupported_.count(n) != 0; }
    std::string emitName(const std::string& n) const {
        return shadowed_.count(n) ? "_shadow_" + n : n;
    }
    GPtr wordNode(const std::string& name) {
        if (shadowed_.count(name)) return gName("_shadow_" + name, false);
        if (callableShadowed_.count(name)) return gName(name, false);
        if (builtins_.count(name)) return gName(name, true);
        return gName(name, false);
    }
    static std::string calleeName(const GPtr& fn) {
        return fn && fn->kind == GNode::Kind::Leaf ? fn->name : std::string();
    }

    struct Result {
        std::vector<GPtr> roots;
        std::size_t next = 0;
    };
    Result parseSeq(const std::vector<Token>& toks) {
        registerAliases(toks);
        return parseRange(toks, 0, false);
    }
    // `alias.infix {<=>} 'name!` / `alias '--> 'name!` register a glyph for
    // the rest of the block, exactly as front.art's registerAliases does;
    // the trailing '!' on the target is a marker, not part of the name
    void registerAliases(const std::vector<Token>& toks) {
        for (std::size_t i = 0; i < toks.size(); ++i) {
            if (toks[i].kind != TK::Word || toks[i].text != "alias") continue;
            std::size_t j = i + 1;
            bool infix = false;
            if (j < toks.size() && toks[j].kind == TK::Attr && toks[j].text == "infix") {
                infix = true;
                ++j;
            }
            if (j + 1 >= toks.size()) continue;
            std::string glyph = toks[j].text;
            std::string target = toks[j + 1].text;
            if (!target.empty() && target.back() == '!') target.pop_back();
            if (glyph.empty() || target.empty()) continue;
            if (infix) infix_[glyph] = target;
            else prefix_[glyph] = target;
        }
    }

    Result parseRange(const std::vector<Token>& toks, std::size_t start, bool stopAtRoot) {
        struct Frame { GPtr node; int needed; };
        std::vector<GPtr> roots;
        std::vector<Frame> stack;
        auto target = [&]() -> std::vector<GPtr>& {
            return stack.empty() ? roots : stack.back().node->args;
        };
        std::function<void()> rewind = [&]() {
            while (!stack.empty() && stack.back().needed <= 0) {
                GPtr done = stack.back().node;
                stack.pop_back();
                if (done->kind == GNode::Kind::Define) onDefine(done);
            }
        };
        std::function<void(GPtr)> appendValue = [&](GPtr v) {
            target().push_back(std::move(v));
            if (!stack.empty()) stack.back().needed--;
            rewind();
        };
        std::function<void(GPtr, int)> descend = [&](GPtr node, int arity) {
            target().push_back(node);
            if (!stack.empty()) stack.back().needed--;  // the frame itself fills one parent slot
            stack.push_back({std::move(node), arity});
            rewind();
        };
        std::function<void(GPtr, const std::vector<Token>&, std::size_t&)> appendInfixAware =
            [&](GPtr v, const std::vector<Token>& ts, std::size_t& i) {
                rewind();
                if (i + 1 < ts.size() && ts[i + 1].kind == TK::Symbol) {
                    auto it = infix_.find(ts[i + 1].text);
                    if (it != infix_.end()) {
                        const BuiltinInfo* b = builtin(it->second);
                        auto call = gCall(aliasTarget(it->second));
                        target().push_back(call);
                        if (!stack.empty()) stack.back().needed--;
                        stack.push_back({call, b ? b->arity : arityOf(it->second)});
                        ++i;  // consume the symbol
                        appendValue(std::move(v));
                        return;
                    }
                }
                appendValue(std::move(v));
            };

        std::size_t i = start;
        while (i < toks.size()) {
            const Token el = toks[i];
            if (el.kind == TK::Label || el.kind == TK::LabelStr) {
                auto d = std::make_shared<GNode>();
                d->kind = GNode::Kind::Define; d->name = el.text;
                descend(d, 1);
            } else if (el.kind == TK::Word) {
                const std::string name = el.text;
                if (name == "true" || name == "false" || name == "null") {
                    appendInfixAware(gLeaf(el), toks, i);
                } else {
                    int ar = arityOf(name);
                    if (ar > 0) descend(gCall(wordNode(name)), ar);
                    else if (ar == 0) appendInfixAware(gCall(wordNode(name)), toks, i);
                    else appendInfixAware(wordNode(name), toks, i);
                }
            } else if (el.kind == TK::Symbol) {
                handleSymbol(el, toks, i, stack, roots, rewind, descend, appendValue, appendInfixAware);
            } else if (el.kind == TK::Path) {
                appendInfixAware(pathGet(el), toks, i);
            } else if (el.kind == TK::PathLabel) {
                auto call = pathSetFrame(el);
                target().push_back(call);
                if (!stack.empty()) stack.back().needed--;
                stack.push_back({call, 1});
            } else if (el.kind == TK::Attr || el.kind == TK::AttrLabel) {
                if (!stack.empty() && stack.back().node->kind == GNode::Kind::Call) {
                    if (el.kind == TK::Attr) {
                        Token t; t.kind = TK::Word; t.text = "true";
                        stack.back().node->attrs.emplace_back(el.text, gName("true", true));
                    } else {
                        auto sub = parseRange(toks, i + 1, true);
                        if (!sub.roots.empty()) {
                            stack.back().node->attrs.emplace_back(el.text, sub.roots[0]);
                            i = sub.next - 1;
                        }
                    }
                } else {
                    appendInfixAware(gLeaf(el), toks, i);
                }
            } else if (el.kind == TK::Block) {
                auto g = std::make_shared<GNode>();
                g->kind = GNode::Kind::BlockRaw; g->token = el;
                appendInfixAware(g, toks, i);
            } else if (el.kind == TK::Inline) {
                auto g = std::make_shared<GNode>();
                g->kind = GNode::Kind::Inline; g->token = el;
                appendInfixAware(g, toks, i);
            } else {
                appendInfixAware(gLeaf(el), toks, i);
            }
            ++i;
            if (stopAtRoot && !roots.empty() && stack.empty()) break;
        }
        return {roots, i};
    }

  private:
    std::unordered_map<std::string, const BuiltinInfo*> builtins_;
    std::unordered_map<std::string, int> arity_;
    std::unordered_map<std::string, std::string> infix_, prefix_;
    std::unordered_set<std::string> mutates_, pure_, unsupported_, shadowable_;
    std::unordered_set<std::string> shadowed_, callableShadowed_;

    int arityOf(const std::string& n) const {
        auto it = arity_.find(n);
        return it == arity_.end() ? -1 : it->second;
    }
    GPtr aliasTarget(const std::string& name) {
        return builtins_.count(name) ? gName(name, true) : gName(name, false);
    }
    void onDefine(const GPtr& d) {
        const std::string name = d->name;
        bool callable = false;
        if (!d->args.empty() && d->args[0]->kind == GNode::Kind::Call) {
            std::string cn = calleeName(d->args[0]->fn);
            callable = cn == "function" || cn == "method" || cn == "constructor" || cn == "sortable";
            if (cn == "function" && !d->args[0]->args.empty()) {
                const auto& pa = d->args[0]->args[0];
                int cnt = 1;
                if (pa->kind == GNode::Kind::BlockRaw) {
                    cnt = 0;
                    for (const auto& t : pa->token.kids) if (t.kind == TK::Word) ++cnt;
                }
                arity_[name] = cnt;
            }
        }
        if (callable && shadowable_.count(name)) callableShadowed_.insert(name);
        if (!callable) {
            arity_.erase(name);
            if (builtins_.count(name)) shadowed_.insert(name);
            d->name = emitName(name);
        }
    }
    GPtr segNode(const Seg& seg) {
        if (seg.isBlock) {
            auto sub = parseSeq(seg.block);
            if (sub.roots.size() == 1) return sub.roots[0];
            auto body = std::make_shared<GNode>();
            body->kind = GNode::Kind::Body; body->args = sub.roots;
            return body;
        }
        Token t;
        bool numeric = !seg.name.empty() &&
                       std::all_of(seg.name.begin(), seg.name.end(),
                                   [](unsigned char c) { return (c >= '0' && c <= '9') || c == '.'; });
        t.kind = seg.isString ? TK::Str : numeric ? (seg.name.find('.') == std::string::npos ? TK::Int : TK::Float)
                                                  : TK::Literal;
        t.text = seg.name;
        return gLeaf(t);
    }
    GPtr pathGet(const Token& el) {
        GPtr node = wordNode(el.segs.empty() ? el.text : el.segs[0].name);
        for (std::size_t j = 1; j < el.segs.size(); ++j) {
            const char* getter = j + 1 == el.segs.size() ? "pathGet" : "get";
            node = gCall(gName(getter, true), {node, segNode(el.segs[j])});
        }
        return node;
    }
    GPtr pathSetFrame(const Token& el) {
        GPtr node = wordNode(el.segs.empty() ? el.text : el.segs[0].name);
        for (std::size_t j = 1; j + 1 < el.segs.size(); ++j)
            node = gCall(gName("get", true), {node, segNode(el.segs[j])});
        GPtr last = el.segs.size() > 1 ? segNode(el.segs.back()) : gLeaf(el);
        return gCall(gName("set", true), {node, last});
    }
    GPtr dictLit(const Token& block) {
        auto d = std::make_shared<GNode>();
        d->kind = GNode::Kind::DictLit;
        const auto& kids = block.kids;
        std::size_t j = 0;
        while (j < kids.size()) {
            if (kids[j].kind != TK::Label && kids[j].kind != TK::LabelStr) { ++j; continue; }
            std::string key = kids[j].text;
            std::size_t vend = j + 1;
            while (vend < kids.size() && kids[vend].kind != TK::Label && kids[vend].kind != TK::LabelStr) ++vend;
            std::vector<Token> vtoks(kids.begin() + static_cast<long>(j + 1), kids.begin() + static_cast<long>(vend));
            GPtr value;
            if (vtoks.empty()) {
                value = gName("null", true);
            } else if (vtoks.size() == 2 && vtoks[0].kind == TK::Symbol && vtoks[0].text == "@" &&
                       vtoks[1].kind == TK::Block) {
                auto g = std::make_shared<GNode>();
                g->kind = GNode::Kind::BlockRaw; g->token = vtoks[1]; g->evaluated = true;
                value = g;
            } else if (vtoks.size() == 2 && vtoks[0].kind == TK::Symbol && vtoks[0].text == "#" &&
                       vtoks[1].kind == TK::Block) {
                value = dictLit(vtoks[1]);
            } else {
                auto sub = parseSeq(vtoks);
                if (sub.roots.size() == 1) value = sub.roots[0];
                else {
                    auto body = std::make_shared<GNode>();
                    body->kind = GNode::Kind::Body; body->args = sub.roots;
                    value = body;
                }
            }
            d->pairs.emplace_back(key, value);
            j = vend;
        }
        return d;
    }

    // `=>` expansion, mirroring front.art's expandThickArrow: the arrow
    // appends TWO values -- a parameter block and an action block -- with
    // `&` placeholders replaced by x1, x2, ... in order of appearance
    template <typename Append>
    void expandThickArrow(const std::vector<Token>& toks, std::size_t& i, Append& appendValue) {
        auto blockNode = [](std::vector<Token> kids) {
            auto g = std::make_shared<GNode>();
            g->kind = GNode::Kind::BlockRaw;
            g->token.kind = TK::Block;
            g->token.kids = std::move(kids);
            return g;
        };
        auto wordTok = [](const std::string& n) {
            Token t; t.kind = TK::Word; t.text = n; return t;
        };
        if (i + 1 >= toks.size()) {
            Token empty; empty.kind = TK::Block;
            appendValue(blockNode({}));
            appendValue(blockNode({}));
            return;
        }
        const Token& nxt = toks[i + 1];
        std::vector<Token> argblock, subblock;
        std::size_t consumed = 1;  // tokens after `=>` that are used up
        if (nxt.kind == TK::Block) {
            int idx = 1;
            for (const auto& el2 : nxt.kids) {
                if (el2.kind == TK::Symbol && el2.text == "&") {
                    Token w = wordTok("x" + std::to_string(idx++));
                    argblock.push_back(w);
                    subblock.push_back(w);
                } else subblock.push_back(el2);
            }
        } else if (nxt.kind == TK::Word) {
            int fnArity = arityOf(nxt.text);
            bool explicitArgs = false, partialArgs = false;
            if (fnArity > 0) {
                for (int j = 1; j <= fnArity && i + 1 + static_cast<std::size_t>(j) < toks.size(); ++j)
                    if (toks[i + 1 + j].kind == TK::Symbol && toks[i + 1 + j].text == "&") explicitArgs = true;
                if (!explicitArgs && fnArity > 1 && i + 2 < toks.size() && toks[i + 2].kind != TK::Word)
                    partialArgs = true;
            }
            if (fnArity > 0) {
                subblock.push_back(nxt);
                if (explicitArgs) {
                    int idx = 1;
                    for (int j = 1; j <= fnArity && i + 1 + static_cast<std::size_t>(j) < toks.size(); ++j) {
                        const Token& cand = toks[i + 1 + j];
                        if (cand.kind == TK::Symbol && cand.text == "&") {
                            Token w = wordTok("x" + std::to_string(idx++));
                            argblock.push_back(w);
                            subblock.push_back(w);
                        } else subblock.push_back(cand);
                    }
                    consumed = 1 + static_cast<std::size_t>(fnArity);
                } else if (partialArgs) {
                    Token w = wordTok("x1");
                    argblock.push_back(w);
                    subblock.push_back(w);
                    for (int j = 1; j < fnArity && i + 1 + static_cast<std::size_t>(j) < toks.size(); ++j)
                        subblock.push_back(toks[i + 1 + j]);
                    consumed = static_cast<std::size_t>(fnArity);
                } else {
                    for (int j = 0; j < fnArity; ++j) {
                        Token w = wordTok("x" + std::to_string(j + 1));
                        argblock.push_back(w);
                        subblock.push_back(w);
                    }
                    consumed = 1;
                }
            } else {
                subblock.push_back(nxt);
                consumed = 1;
            }
        } else {
            subblock.push_back(nxt);
            consumed = 1;
        }
        appendValue(blockNode(argblock));
        appendValue(blockNode(subblock));
        i += consumed;
    }

    template <typename Stack, typename Rewind, typename Descend, typename Append, typename AppendInfix>
    void handleSymbol(const Token& el, const std::vector<Token>& toks, std::size_t& i,
                      Stack& stack, std::vector<GPtr>& roots, Rewind& rewind,
                      Descend& descend, Append& appendValue, AppendInfix& appendInfixAware) {
        const std::string& sym = el.text;
        (void)roots;
        if (sym == "=>") {
            expandThickArrow(toks, i, appendValue);
            return;
        }
        if (sym == "->") {
            auto sub = parseRange(toks, i + 1, true);
            auto body = std::make_shared<GNode>();
            body->kind = GNode::Kind::Body; body->args = sub.roots;
            appendInfixAware(body, toks, i);
            if (sub.next > 0) i = sub.next - 1;
            return;
        }
        if (sym == "$") { descend(gCall(gName("function", true)), 2); return; }
        if (sym == "#") {
            if (i + 1 < toks.size() && toks[i + 1].kind == TK::Block) {
                auto d = dictLit(toks[i + 1]);
                ++i;
                appendInfixAware(d, toks, i);
            } else {
                appendInfixAware(gLeaf(el), toks, i);
            }
            return;
        }
        if (sym == "@") {
            if (i + 1 < toks.size() && toks[i + 1].kind == TK::Block) {
                auto g = std::make_shared<GNode>();
                g->kind = GNode::Kind::BlockRaw; g->token = toks[i + 1]; g->evaluated = true;
                ++i;
                appendInfixAware(g, toks, i);
            } else {
                auto sub = parseRange(toks, i + 1, true);
                if (!sub.roots.empty()) {
                    Token typeTok; typeTok.kind = TK::TypeTok; typeTok.text = "block";
                    auto conv = gCall(gName("to", true), {gLeaf(typeTok), sub.roots[0]});
                    appendInfixAware(conv, toks, i);
                    if (sub.next > 0) i = sub.next - 1;
                }
            }
            return;
        }
        auto pre = prefix_.find(sym);
        if (pre != prefix_.end()) {
            int ar = arityOf(pre->second);
            if (ar > 0) descend(gCall(aliasTarget(pre->second)), ar);
            else if (ar == 0) appendInfixAware(gCall(aliasTarget(pre->second)), toks, i);
            else appendInfixAware(aliasTarget(pre->second), toks, i);
            return;
        }
        (void)stack; (void)rewind; (void)appendValue;
        appendInfixAware(gLeaf(el), toks, i);
    }
};

}  // namespace artc
