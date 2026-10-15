// lexer.hpp -- Arturo source lexer for the C++26 compiler.
//
// Token kinds mirror the host lexer's (`to :block`): words, labels, literals,
// types, symbols, attributes, paths, blocks and inlines. Plain strings do not
// interpolate in Arturo 0.10.0 (verified against the pinned host: "Hello
// {name}!" prints literally), so strings are plain constants here.
#pragma once

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ir.hpp"

namespace artc {

struct CompileError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

enum class TK {
    End, Int, Float, Str, CharTok, Word, Label, LabelStr, Literal, TypeTok,
    Symbol, Attr, AttrLabel, Path, PathLabel, Block, Inline,
    Quantity, UnitTok, VersionTok, ColorTok, RegexTok, SymbolLit, RationalTok,
};

struct Seg {
    std::string name;                 // word/number/string segment spelling
    bool isBlock = false;             // segment written as [...]
    bool isString = false;            // segment written as "..."
    std::vector<struct Token> block;  // tokens inside [...]
};

struct Token {
    TK kind = TK::End;
    std::string text;              // spelling / decoded string / char bytes
    std::string unit;              // Quantity: unit spelling after the backtick
    std::vector<Token> kids;       // Block / Inline contents
    std::vector<Seg> segs;         // Path / PathLabel / literal-path segments
    bool literalPath = false;      // 'a\b  (quoted path)
    int line = 1;
};

inline bool isWordStart(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80;
}
inline bool isWordChar(unsigned char c) {
    return isWordStart(c) || (c >= '0' && c <= '9');
}
inline bool isDigit(unsigned char c) { return c >= '0' && c <= '9'; }
inline bool isSymbolChar(unsigned char c) {
    switch (c) {
        case '~': case '!': case '@': case '#': case '$': case '%': case '^':
        case '&': case '*': case '-': case '=': case '+': case '<': case '>':
        case '/': case '|': case '?':
            return true;
        default: return c >= 0x80;
    }
}

class Lexer {
  public:
    explicit Lexer(std::string src) : s_(std::move(src)) {}

    std::vector<Token> run() {
        auto toks = lexSeq(false, '\0');
        return toks;
    }

  private:
    std::string s_;
    std::size_t pos_ = 0;
    int line_ = 1;
    int depth_ = 0;

    [[noreturn]] void fail(const std::string& msg) const {
        throw CompileError("lexer: " + msg + " (line " + std::to_string(line_) + ")");
    }
    unsigned char peek(std::size_t off = 0) const {
        return pos_ + off < s_.size() ? static_cast<unsigned char>(s_[pos_ + off]) : 0;
    }
    unsigned char take() {
        unsigned char c = peek();
        if (pos_ < s_.size()) ++pos_;
        if (c == '\n') ++line_;
        return c;
    }
    void skipSpaceAndComments() {
        for (;;) {
            while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' ||
                                        s_[pos_] == '\r' || s_[pos_] == '\n' || s_[pos_] == ',')) take();
            if (peek() == ';') {
                while (pos_ < s_.size() && peek() != '\n') take();
                continue;
            }
            break;
        }
    }
    std::string readWord() {
        std::string w;
        while (isWordChar(peek())) w += static_cast<char>(take());
        if (peek() == '?' || peek() == '!') w += static_cast<char>(take());
        return w;
    }
    std::string readUtf8Char() {
        std::string out;
        unsigned char c = peek();
        if (c == 0) fail("unterminated character literal");
        int n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        for (int i = 0; i < n && pos_ < s_.size(); ++i) out += static_cast<char>(take());
        return out;
    }
    char decodeEscape() {
        char e = static_cast<char>(take());
        switch (e) {
            case 'n': return '\n';
            case 't': return '\t';
            case 'r': return '\r';
            case '0': return '\0';
            default: return e;
        }
    }
    Token lexString() {
        Token t; t.kind = TK::Str; t.line = line_;
        take();  // opening "
        // the host lexer toggles a code mode at each '|': inside template
        // segments, backslash escapes are NOT decoded (so |this\name| keeps
        // its path separator for render); outside, they are
        bool inCode = false;
        while (pos_ < s_.size() && peek() != '"') {
            unsigned char c = peek();
            if (c == '|') { inCode = !inCode; t.text += static_cast<char>(take()); continue; }
            if (c == '\\' && !inCode) {
                take();
                char e = static_cast<char>(peek());
                switch (e) {
                    case '\\': case '"': case '\'': case '/': t.text += decodeEscape(); break;
                    case 'a': take(); t.text += '\a'; break;
                    case 'b': take(); t.text += '\b'; break;
                    case 'e': take(); t.text += '\x1b'; break;
                    case 'f': take(); t.text += '\f'; break;
                    case 'n': case 'l': take(); t.text += '\n'; break;
                    case 'r': case 'c': take(); t.text += '\r'; break;
                    case 't': take(); t.text += '\t'; break;
                    case 'v': take(); t.text += '\v'; break;
                    default:  // unknown escape: the backslash survives (host rule)
                        t.text += '\\';
                        t.text += static_cast<char>(take());
                        break;
                }
                continue;
            }
            t.text += static_cast<char>(take());
        }
        if (pos_ >= s_.size()) fail("unterminated string");
        take();  // closing "
        if (peek() == ':') { take(); t.kind = TK::LabelStr; }
        return t;
    }
    // port of the runtime lexer's dedent_curly_text: drop the common leading
    // indentation, then trim surrounding whitespace
    static std::string dedent(const std::string& input) {
        std::vector<std::string> lines;
        std::string cur;
        for (char ch : input) {
            if (ch == '\n') { lines.push_back(cur); cur.clear(); }
            else cur += ch;
        }
        lines.push_back(cur);
        std::size_t minimum = std::string::npos;
        for (const auto& l : lines) {
            std::size_t p = 0;
            while (p < l.size() && (l[p] == ' ' || l[p] == '\t')) ++p;
            if (p < l.size()) minimum = std::min(minimum, p);
        }
        if (minimum == std::string::npos) minimum = 0;
        std::string out;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            std::size_t drop = 0;
            while (drop < lines[i].size() && drop < minimum &&
                   (lines[i][drop] == ' ' || lines[i][drop] == '\t')) ++drop;
            out += lines[i].substr(drop);
            if (i + 1 < lines.size()) out += '\n';
        }
        std::size_t b = out.find_first_not_of(" \t\r\n");
        std::size_t e = out.find_last_not_of(" \t\r\n");
        return b == std::string::npos ? "" : out.substr(b, e - b + 1);
    }
    Token lexBraceString() {
        int ln = line_;
        take();  // {
        bool verbatim = false, regex = false;
        if (peek() == '!') {  // {!flags ...}: flag letters are skipped
            take();
            while (isWordChar(peek())) take();
        }
        if (peek() == ':') { verbatim = true; take(); }
        else if (peek() == '/') { regex = true; take(); }
        std::string raw;
        if (verbatim) {
            while (pos_ < s_.size() && !(peek() == ':' && peek(1) == '}')) raw += static_cast<char>(take());
            if (pos_ >= s_.size()) fail("unterminated {:...:} string");
            take(); take();
        } else {
            int depth = 1;
            while (pos_ < s_.size() && depth > 0) {
                unsigned char c = peek();
                if (c == '{') { ++depth; raw += static_cast<char>(take()); }
                else if (c == '}') {
                    --depth;
                    if (depth == 0) { take(); break; }
                    raw += static_cast<char>(take());
                } else if (regex && c == '/' && peek(1) == '}') {
                    take();  // pattern terminator; '}' closes next iteration
                } else raw += static_cast<char>(take());
            }
            if (depth != 0) fail("unterminated {...} string");
        }
        Token t; t.line = ln;
        if (regex) { t.kind = TK::RegexTok; t.text = raw; }
        else { t.kind = TK::Str; t.text = verbatim ? raw : dedent(raw); }
        return t;
    }
    Token lexSafeString() {
        Token t; t.kind = TK::Str; t.line = line_;
        if (!(peek(2) == 0xC2 && peek(3) == 0xAB)) {
            // single «: the rest of the line is the string (host rule)
            pos_ += 2;
            while (peek() == ' ' || peek() == '\t') take();
            while (pos_ < s_.size() && peek() != '\n' && peek() != '\r') t.text += static_cast<char>(take());
            while (!t.text.empty() && (t.text.back() == ' ' || t.text.back() == '\t')) t.text.pop_back();
            return t;
        }
        pos_ += 4;  // «« (two 2-byte guillemets)
        while (pos_ < s_.size()) {
            if (peek() == 0xC2 && peek(1) == 0xBB && peek(2) == 0xC2 && peek(3) == 0xBB) {
                pos_ += 4;
                return t;
            }
            if (peek() == '\r') { take(); if (peek() == '\n') take(); t.text += '\n'; }
            else t.text += static_cast<char>(take());
        }
        fail("unterminated ««...»» string");
    }
    // path segments after a base name; consumes `\seg` repeatedly
    void readSegs(Token& t) {
        while (peek() == '\\') {
            take();
            Seg seg;
            if (peek() == '[') {
                take();
                seg.isBlock = true;
                seg.block = lexSeq(true, ']');
            } else if (peek() == '"') {
                seg.isString = true;
                seg.name = lexString().text;
            } else {
                while (isWordChar(peek()) || peek() == '?') seg.name += static_cast<char>(take());
                if (seg.name.empty()) fail("empty path segment");
            }
            t.segs.push_back(std::move(seg));
        }
    }
    // glyph-accurate symbol reading, mirroring the runtime lexer's
    // lx_symbol table (multi-char glyphs are matched longest-first)
    std::string readSymbol() {
        unsigned char c = peek();
        if (c >= 0x80) return readUtf8Char();  // one unicode glyph per symbol
        static const std::vector<std::string> kGlyphs = {
            "->>", "-<<", "-->", "==>", "<=>", "<==", "<->>", "<<<", "<~>", "<|>", "###",
            "~>", "!!", "??", "**", "||", "|-", "|=", "|>", "//", "/%", "/\\", "++",
            "->", "-<", "-:", "--", "=~", "=>>", "=>", "=<", "==", "<=", "<->", "<-",
            "<>", "<<", "<~", "<|", "<:", ">=", ">-<<", ">-<", ">>", ">:",
            "##", "~", "!", "?", "@", "#", "$", "%", "^", "&", "*", "|", "/", "+",
            "-", "=", "<", ">",
        };
        for (const auto& g : kGlyphs) {
            if (s_.compare(pos_, g.size(), g) == 0) {
                pos_ += g.size();
                return g;
            }
        }
        return std::string(1, static_cast<char>(take()));
    }
    Token lexQuoted() {
        int ln = line_;
        take();  // '
        // char literal: '<char>' or '<escape>'
        std::size_t save = pos_;
        int saveLine = line_;
        std::string ch;
        if (peek() == '\\') { take(); ch = decodeEscape(); }
        else if (peek() != 0 && peek() != '\'') ch = readUtf8Char();
        if (!ch.empty() && peek() == '\'') {
            take();
            Token t; t.kind = TK::CharTok; t.text = ch; t.line = ln;
            return t;
        }
        pos_ = save; line_ = saveLine;
        Token t; t.line = ln;
        if (isSymbolChar(peek())) {  // symbol literal: '++  '->>  ...
            t.text = readSymbol();
            t.kind = TK::SymbolLit;
            return t;
        }
        std::string name = readWord();
        if (name.empty()) fail("expected a name after '");
        Seg base; base.name = name;
        t.segs.push_back(base);
        readSegs(t);
        t.kind = TK::Literal;
        t.text = name;
        t.literalPath = t.segs.size() > 1;
        return t;
    }
    Token lexNumber() {
        Token t; t.line = line_;
        std::string n;
        while (isDigit(peek()) || peek() == '_') n += static_cast<char>(take());
        // hexadecimal / binary integers: 0xE8, 0b101
        if (n == "0" && (peek() == 'x' || peek() == 'X' || peek() == 'b' || peek() == 'B')) {
            bool hex = peek() == 'x' || peek() == 'X';
            take();
            std::string digits;
            while (isDigit(peek()) || (hex && ((peek() >= 'a' && peek() <= 'f') || (peek() >= 'A' && peek() <= 'F'))))
                digits += static_cast<char>(take());
            unsigned __int128 v = 0;
            for (char d : digits) {
                int val = d >= '0' && d <= '9' ? d - '0' : d >= 'a' && d <= 'f' ? d - 'a' + 10 : d - 'A' + 10;
                v = v * (hex ? 16 : 2) + static_cast<unsigned>(val);
            }
            std::string dec;
            if (v == 0) dec = "0";
            while (v > 0) { dec = static_cast<char>('0' + v % 10) + dec; v /= 10; }
            t.kind = TK::Int;
            t.text = dec;
            return t;
        }
        // a rational literal: 1:2 (also a quantity amount: 127:5000`meters)
        if (peek() == ':' && isDigit(peek(1))) {
            take();
            std::string den;
            while (isDigit(peek())) den += static_cast<char>(take());
            if (peek() == '`') {
                take();
                std::string unit;
                while (isWordChar(peek()) || peek() == '/' || peek() == '*' || peek() == '.')
                    unit += static_cast<char>(take());
                t.kind = TK::Quantity;
                t.text = n + "/" + den;
                t.unit = unit;
                return t;
            }
            t.kind = TK::RationalTok;
            t.text = n + "/" + den;
            return t;
        }
        bool isFloat = false;
        if (peek() == '.' && isDigit(peek(1))) {
            isFloat = true;
            n += static_cast<char>(take());
            while (isDigit(peek())) n += static_cast<char>(take());
        }
        if (peek() == 'e' || peek() == 'E') {
            std::size_t save = pos_;
            n += static_cast<char>(take());
            if (peek() == '+' || peek() == '-') n += static_cast<char>(take());
            if (isDigit(peek())) { isFloat = true; while (isDigit(peek())) n += static_cast<char>(take()); }
            else pos_ = save;
        }
        // a second dotted component makes a version: 1.0.2, 1.2.3-rc1+build
        if (isFloat && peek() == '.' && isDigit(peek(1))) {
            n += static_cast<char>(take());
            while (isDigit(peek())) n += static_cast<char>(take());
            if (peek() == '-' || peek() == '+') {
                while (isWordChar(peek()) || peek() == '.' || peek() == '+' || peek() == '-')
                    n += static_cast<char>(take());
            }
            t.kind = TK::VersionTok;
            t.text = n;
            return t;
        }
        // a backtick suffix makes a quantity: 3`m  30`N  1.5`km/h
        if (peek() == '`') {
            take();
            std::string unit;
            while (isWordChar(peek()) || peek() == '/' || peek() == '*' || peek() == '.')
                unit += static_cast<char>(take());
            t.kind = TK::Quantity;
            t.text = n;
            t.unit = unit;
            return t;
        }
        t.kind = isFloat ? TK::Float : TK::Int;
        t.text = n;
        return t;
    }
    std::vector<Token> lexSeq(bool nested, char close) {
        // Recursive descent on [...] / (...) nesting: reject pathological
        // depth with a diagnostic instead of overflowing the stack.
        if (++depth_ > 8192) fail("blocks nested too deeply (limit 8192)");
        std::vector<Token> out;
        for (;;) {
            skipSpaceAndComments();
            if (pos_ >= s_.size()) {
                if (nested) fail(std::string("unterminated block, expected ") + close);
                break;
            }
            unsigned char c = peek();
            if (nested && c == close) { take(); break; }
            if (c == ']' || c == ')') fail(std::string("unbalanced ") + static_cast<char>(c));
            out.push_back(lexOne());
        }
        --depth_;
        return out;
    }
    Token lexOne() {
        unsigned char c = peek();
        int ln = line_;
        if (c == 0xC3 && peek(1) == 0xB8) {  // ø is the null literal
            pos_ += 2;
            Token t; t.kind = TK::Word; t.text = "null"; t.line = ln;
            return t;
        }
        if (c == 0xE2 && peek(1) == 0x88 && peek(2) == 0x9E) {  // ∞ is infinity
            pos_ += 3;
            Token t; t.kind = TK::Float; t.text = "inf"; t.line = ln;
            return t;
        }
        if (c == 0xC2 && peek(1) == 0xAB) return lexSafeString();  // « or ««
        if (c == '-' && peek(1) == '-' && peek(2) == '-') {
            // `---` dash string: everything to EOF, dedented (host rule)
            Token t; t.kind = TK::Str; t.line = ln;
            while (peek() == '-') take();
            std::string raw;
            while (pos_ < s_.size()) raw += static_cast<char>(take());
            t.text = dedent(raw);
            return t;
        }
        if (c == '"') return lexString();
        if (c == '{') return lexBraceString();
        if (c == '`') {  // bare unit literal: `m
            take();
            Token t; t.kind = TK::UnitTok; t.line = ln;
            while (isWordChar(peek()) || peek() == '/' || peek() == '*' || peek() == '.')
                t.text += static_cast<char>(take());
            if (t.text.empty()) fail("expected unit after `");
            return t;
        }
        if (c == '#' && isWordChar(peek(1))) {  // color literal: #blue  #FF8800
            take();
            Token t; t.kind = TK::ColorTok; t.line = ln;
            while (isWordChar(peek())) t.text += static_cast<char>(take());
            return t;
        }
        if (c == '\'') return lexQuoted();
        if (c == '[') { take(); Token t; t.kind = TK::Block; t.line = ln; t.kids = lexSeq(true, ']'); return t; }
        if (c == '(') { take(); Token t; t.kind = TK::Inline; t.line = ln; t.kids = lexSeq(true, ')'); return t; }
        if (isDigit(c)) return lexNumber();
        if (c == '.') {
            if (isDigit(peek(1))) return lexNumber();
            if (peek(1) == '.') {
                take(); take();
                Token t; t.kind = TK::Symbol; t.text = ".."; t.line = ln; return t;
            }
            take();
            Token t; t.line = ln;
            t.text = readWord();
            if (t.text.empty()) fail("expected attribute name after .");
            if (peek() == ':') { take(); t.kind = TK::AttrLabel; }
            else t.kind = TK::Attr;
            return t;
        }
        if (c == ':') {
            if (peek(1) == '=') {  // the := symbol
                take(); take();
                Token t; t.kind = TK::Symbol; t.text = ":="; t.line = ln;
                return t;
            }
            take();
            Token t; t.kind = TK::TypeTok; t.line = ln;
            t.text = readWord();
            if (t.text.empty()) fail("expected type name after :");
            return t;
        }
        if (c == '\\') {  // a leading-backslash path is rooted at `this`
            Token t; t.line = ln;
            Seg base; base.name = "this";
            t.segs.push_back(base);
            readSegs(t);
            if (peek() == ':' && peek(1) != ':') { take(); t.kind = TK::PathLabel; }
            else t.kind = TK::Path;
            t.text = "this";
            return t;
        }
        if (isWordStart(c) && !isSymbolChar(c)) {
            Token t; t.line = ln;
            std::string w = readWord();
            if (w == "_") {  // a lone underscore is a symbol on the host
                t.kind = TK::Symbol; t.text = "_";
                return t;
            }
            if (peek() == '\\') {
                Seg base; base.name = w;
                t.segs.push_back(base);
                readSegs(t);
                if (peek() == ':' && peek(1) != ':') { take(); t.kind = TK::PathLabel; }
                else t.kind = TK::Path;
                t.text = w;
                return t;
            }
            if (peek() == ':' && peek(1) != ':') {
                take();
                t.kind = TK::Label; t.text = w;
                return t;
            }
            t.kind = TK::Word; t.text = w;
            return t;
        }
        if (isSymbolChar(c)) {
            Token t; t.kind = TK::Symbol; t.line = ln;
            t.text = readSymbol();
            return t;
        }
        fail(std::string("unexpected character '") + static_cast<char>(c) + "'");
    }
};

}  // namespace artc
