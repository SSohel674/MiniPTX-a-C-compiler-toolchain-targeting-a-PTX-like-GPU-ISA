#include "assembler.h"
#include <cctype>
#include <map>
#include <sstream>
#include "common.h"

namespace mp {
namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}
std::vector<std::string> split(const std::string& s, char d) {
    std::vector<std::string> v;
    std::string cur;
    for (char c : s) { if (c == d) { v.push_back(cur); cur.clear(); } else cur += c; }
    v.push_back(cur);
    return v;
}
bool startsWith(const std::string& s, const char* p) { return s.compare(0, std::strlen(p), p) == 0; }

struct Header {
    bool entry = false, extern_ = false;
    uint8_t retCls = C_VOID;
    std::string name;
    std::vector<std::pair<std::string, uint8_t>> params;
};

class Assembler {
public:
    Object run(const std::string& text) {
        std::istringstream is(text);
        std::string raw;
        while (std::getline(is, raw)) {
            ln_++;
            size_t c = raw.find("//");
            if (c != std::string::npos) raw = raw.substr(0, c);
            std::string line = trim(raw);
            if (line.empty()) continue;
            handleLine(line);
        }
        if (inFunc_) err("missing '}' at end of input");
        return std::move(obj_);
    }

private:
    Object obj_;
    int ln_ = 0;
    bool inFunc_ = false, expectBrace_ = false;
    int fileIdx_ = -1;
    // current function state
    int sym_ = -1;
    uint32_t base_ = 0;
    int nregs_[4] = {0, 0, 0, 0};
    std::vector<std::pair<std::string, uint8_t>> params_;
    std::map<std::string, uint32_t> labels_;
    struct Patch { uint32_t instr; int slot; std::string label; int line; };
    std::vector<Patch> patches_;
    uint32_t curLine_ = 0;
    bool lineDirty_ = true;

    [[noreturn]] void err(const std::string& m) const { throw CompileError(m, ln_); }

    static int clsFromTok(const std::string& t) {
        if (t == "s32" || t == ".s32") return C_S32;
        if (t == "f32" || t == ".f32") return C_F32;
        if (t == "u64" || t == ".u64") return C_U64;
        if (t == "pred" || t == ".pred") return C_PRED;
        if (t == "void") return C_VOID;
        return -2;
    }
    int needCls(const std::string& t) const {
        int c = clsFromTok(t);
        if (c == -2) err("unknown type '" + t + "'");
        return c;
    }

    Header parseHeader(std::string line) {
        if (!line.empty() && line.back() == ';') line.pop_back();
        std::string spaced;
        for (char c : line) { if (c == '(' || c == ')' || c == ',') { spaced += ' '; spaced += c; spaced += ' '; } else spaced += c; }
        std::istringstream ts(spaced);
        std::vector<std::string> t;
        std::string x;
        while (ts >> x) t.push_back(x);
        Header h;
        size_t i = 0;
        auto need = [&](const char* s) { if (i >= t.size() || t[i] != s) err(std::string("malformed function header: expected '") + s + "'"); i++; };
        while (i < t.size() && (t[i] == ".visible" || t[i] == ".extern")) { if (t[i] == ".extern") h.extern_ = true; i++; }
        if (i < t.size() && t[i] == ".entry") { h.entry = true; i++; }
        else if (i < t.size() && t[i] == ".func") {
            i++;
            if (i < t.size() && t[i] == "(") {
                i++; need(".param");
                if (i >= t.size()) err("malformed return parameter");
                h.retCls = (uint8_t)needCls(t[i++]);
                i++;  // return value name
                need(")");
            }
        } else err("malformed function header");
        if (i >= t.size()) err("missing function name");
        h.name = t[i++];
        need("(");
        while (i < t.size() && t[i] != ")") {
            need(".param");
            if (i + 1 >= t.size()) err("malformed parameter list");
            uint8_t c = (uint8_t)needCls(t[i++]);
            h.params.push_back({t[i++], c});
            if (i < t.size() && t[i] == ",") i++;
        }
        need(")");
        if (h.params.size() > 8) err("too many parameters");
        return h;
    }

    int symFor(const std::string& name) {
        int s = obj_.findSym(name);
        if (s >= 0) return s;
        Symbol x; x.name = name; x.kind = SYM_UNDEF;
        obj_.syms.push_back(x);
        return (int)obj_.syms.size() - 1;
    }

    void fillSym(Symbol& s, const Header& h) {
        s.retCls = h.retCls;
        s.nparams = (uint8_t)h.params.size();
        for (size_t k = 0; k < h.params.size(); k++) s.paramCls[k] = h.params[k].second;
    }

    void handleLine(const std::string& line) {
        if (startsWith(line, ".version") || startsWith(line, ".target")) return;
        if (startsWith(line, ".file")) {
            size_t a = line.find('"'), b = line.rfind('"');
            if (a == std::string::npos || b <= a) err("malformed .file directive");
            std::string f = line.substr(a + 1, b - a - 1);
            for (size_t i = 0; i < obj_.files.size(); i++) if (obj_.files[i] == f) { fileIdx_ = (int)i; return; }
            obj_.files.push_back(f);
            fileIdx_ = (int)obj_.files.size() - 1;
            return;
        }
        if (!inFunc_) {
            if (line == "{") { if (!expectBrace_) err("unexpected '{'"); inFunc_ = true; expectBrace_ = false; return; }
            if (startsWith(line, ".extern")) {
                Header h = parseHeader(line);
                int s = symFor(h.name);
                if (obj_.syms[s].kind == SYM_UNDEF) fillSym(obj_.syms[s], h);
                return;
            }
            if (startsWith(line, ".visible") || startsWith(line, ".func") || startsWith(line, ".entry")) {
                Header h = parseHeader(line);
                int s = symFor(h.name);
                Symbol& sy = obj_.syms[s];
                if (sy.kind != SYM_UNDEF) err("duplicate definition of '" + h.name + "'");
                sy.kind = h.entry ? SYM_KERNEL : SYM_DEVICE;
                sy.value = (uint32_t)obj_.text.size();
                fillSym(sy, h);
                sym_ = s; base_ = sy.value; params_ = h.params;
                for (int k = 0; k < 4; k++) nregs_[k] = 0;
                labels_.clear(); patches_.clear(); lineDirty_ = true; curLine_ = 0;
                expectBrace_ = true;
                return;
            }
            err("unexpected text outside a function: '" + line + "'");
        }
        if (line == "}") { endFunc(); return; }
        if (startsWith(line, ".reg")) { parseRegDecl(line); return; }
        if (startsWith(line, ".loc")) {
            std::istringstream ss(line.substr(4));
            long v; if (!(ss >> v) || v < 0) err("malformed .loc");
            curLine_ = (uint32_t)v; lineDirty_ = true;
            return;
        }
        if (startsWith(line, ".var")) { parseVar(line); return; }
        if (line.back() == ':') {
            std::string name = trim(line.substr(0, line.size() - 1));
            if (labels_.count(name)) err("duplicate label '" + name + "'");
            labels_[name] = (uint32_t)obj_.text.size();
            return;
        }
        parseInstr(line);
    }

    void endFunc() {
        Symbol& sy = obj_.syms[sym_];
        sy.size = (uint32_t)obj_.text.size() - base_;
        for (int k = 0; k < 4; k++) sy.nregs[k] = (uint16_t)nregs_[k];
        for (auto& p : patches_) {
            auto it = labels_.find(p.label);
            if (it == labels_.end()) throw CompileError("undefined label '" + p.label + "'", p.line);
            Operand& o = obj_.text[p.instr].s[p.slot];
            o = mkOp(K_LABEL, 0, 0, (int32_t)it->second);
            Reloc r; r.instr = p.instr; r.slot = (uint8_t)p.slot; r.sym = -1; r.addend = (int32_t)it->second;
            obj_.relocs.push_back(r);
        }
        inFunc_ = false;
        sym_ = -1;
    }

    static int regClass(const std::string& prefix) {
        if (prefix == "r") return C_S32;
        if (prefix == "f") return C_F32;
        if (prefix == "p") return C_PRED;
        if (prefix == "rd") return C_U64;
        return -1;
    }

    void parseRegDecl(const std::string& line) {  // .reg .s32 %r<6>;
        std::string s = line.substr(4);
        if (!s.empty() && s.back() == ';') s.pop_back();
        std::istringstream ss(s);
        std::string ty, rg;
        if (!(ss >> ty >> rg)) err("malformed .reg");
        size_t lt = rg.find('<'), gt = rg.find('>');
        if (rg[0] != '%' || lt == std::string::npos || gt == std::string::npos) err("malformed .reg");
        int cls = regClass(rg.substr(1, lt - 1));
        if (cls < 0 || cls != needCls(ty)) err("register prefix does not match type in .reg");
        nregs_[cls] = std::atoi(rg.substr(lt + 1, gt - lt - 1).c_str());
    }

    void parseVar(const std::string& line) {  // .var %r1, "i", lo, hi;
        std::string s = line.substr(4);
        if (!s.empty() && s.back() == ';') s.pop_back();
        size_t q1 = s.find('"'), q2 = s.rfind('"');
        if (q1 == std::string::npos || q2 <= q1) err("malformed .var");
        std::string reg = trim(s.substr(0, s.find(',')));
        std::string name = s.substr(q1 + 1, q2 - q1 - 1);
        auto rest = split(s.substr(q2 + 1), ',');
        if (rest.size() < 3) err("malformed .var");
        Operand r = parseReg(reg);
        VarEnt v; v.sym = (uint32_t)sym_; v.cls = r.cls; v.idx = r.idx; v.name = name;
        v.lo = base_ + (uint32_t)std::atoi(trim(rest[1]).c_str());
        v.hi = base_ + (uint32_t)std::atoi(trim(rest[2]).c_str());
        obj_.vars.push_back(v);
    }

    Operand parseReg(const std::string& t) const {
        if (t.size() < 3 || t[0] != '%') err("expected register, found '" + t + "'");
        size_t i = 1;
        while (i < t.size() && std::isalpha((unsigned char)t[i])) i++;
        int cls = regClass(t.substr(1, i - 1));
        if (cls < 0 || i >= t.size()) err("bad register '" + t + "'");
        for (size_t k = i; k < t.size(); k++) if (!std::isdigit((unsigned char)t[k])) err("bad register '" + t + "'");
        int idx = std::atoi(t.c_str() + i);
        if (idx >= nregs_[cls]) err("register '" + t + "' not declared (.reg ." + clsName(cls) + " %" + regPrefix(cls) + "<N>)");
        return mkReg(idx, (uint8_t)cls);
    }

    Operand parseOp(const std::string& t, uint8_t immCls) const {
        if (t.empty()) err("missing operand");
        if (t[0] == '%') {
            static const char* sp[] = {"%tid.x", "%ctaid.x", "%ntid.x", "%nctaid.x"};
            for (int k = 0; k < 4; k++) if (t == sp[k]) return mkOp(K_SPECIAL, C_S32, 0, k);
            return parseReg(t);
        }
        if (startsWith(t, "0f") && t.size() == 10) {
            uint32_t bits = (uint32_t)std::strtoul(t.c_str() + 2, nullptr, 16);
            return mkImm((int32_t)bits, C_F32);
        }
        char* end = nullptr;
        long v = std::strtol(t.c_str(), &end, 10);
        if (*end != '\0') err("bad operand '" + t + "'");
        return mkImm((int32_t)v, immCls);
    }

    void parseInstr(std::string line) {
        if (line.back() == ';') line.pop_back();
        line = trim(line);
        Instr I;
        if (line[0] == '@') {
            size_t sp = line.find_first_of(" \t");
            if (sp == std::string::npos) err("predicate without instruction");
            std::string p = line.substr(1, sp - 1);
            I.predmode = 1;
            if (!p.empty() && p[0] == '!') { I.predmode = 2; p = p.substr(1); }
            I.pred = parseReg(p);
            if (I.pred.cls != C_PRED) err("predicate must be a %p register");
            line = trim(line.substr(sp));
        }
        size_t sp = line.find_first_of(" \t");
        std::string mn = sp == std::string::npos ? line : line.substr(0, sp);
        std::vector<std::string> ops;
        if (sp != std::string::npos) {
            for (auto& o : split(line.substr(sp), ',')) {
                std::string t = trim(o);
                if (!t.empty() && t[0] == '[' && t.back() == ']') t = trim(t.substr(1, t.size() - 2));
                ops.push_back(t);
            }
        }
        auto parts = split(mn, '.');
        const std::string& m = parts[0];
        auto nops = [&](size_t n) { if (ops.size() != n) err("'" + mn + "' expects " + std::to_string(n) + " operand(s)"); };
        auto nparts = [&](size_t n) { if (parts.size() != n) err("malformed mnemonic '" + mn + "'"); };
        auto destReg = [&](const std::string& t) { return parseReg(t); };
        auto simple = [&](Op op, size_t nsrc) {
            nparts(2); nops(nsrc + 1);
            I.op = op; I.type = (uint8_t)needCls(parts[1]);
            I.dst = destReg(ops[0]);
            for (size_t k = 0; k < nsrc; k++) I.s[k] = parseOp(ops[k + 1], I.type);
        };
        int slot = -1;
        std::string patchLabel;
        if (m == "mov") simple(OP_MOV, 1);
        else if (m == "add") simple(OP_ADD, 2);
        else if (m == "sub") simple(OP_SUB, 2);
        else if (m == "mul" && parts.size() == 3 && parts[1] == "wide") {
            nops(3); I.op = OP_MUL_WIDE; I.type = C_U64;
            I.dst = destReg(ops[0]); I.s[0] = parseOp(ops[1], C_S32); I.s[1] = parseOp(ops[2], C_S32);
        }
        else if (m == "mul") simple(OP_MUL, 2);
        else if (m == "div") simple(OP_DIV, 2);
        else if (m == "rem") simple(OP_REM, 2);
        else if (m == "neg") simple(OP_NEG, 1);
        else if (m == "and") simple(OP_AND, 2);
        else if (m == "or") simple(OP_OR, 2);
        else if (m == "not") simple(OP_NOT, 1);
        else if (m == "setp") {
            nparts(3); nops(3);
            static const char* cn[] = {"lt", "le", "gt", "ge", "eq", "ne"};
            int c = -1;
            for (int k = 0; k < 6; k++) if (parts[1] == cn[k]) c = k;
            if (c < 0) err("unknown comparison '" + parts[1] + "'");
            I.op = OP_SETP; I.cmp = (uint8_t)c; I.type = (uint8_t)needCls(parts[2]);
            I.dst = destReg(ops[0]); I.s[0] = parseOp(ops[1], I.type); I.s[1] = parseOp(ops[2], I.type);
        }
        else if (m == "cvt") {
            nparts(3); nops(2);
            I.op = OP_CVT; I.type = (uint8_t)needCls(parts[1]); I.cmp = (uint8_t)needCls(parts[2]);
            I.dst = destReg(ops[0]); I.s[0] = parseOp(ops[1], I.cmp);
        }
        else if (m == "ld") {
            nparts(3); nops(2);
            I.type = (uint8_t)needCls(parts[2]);
            I.dst = destReg(ops[0]);
            if (parts[1] == "param") {
                I.op = OP_LD_PARAM;
                int pi = -1;
                for (size_t k = 0; k < params_.size(); k++) if (params_[k].first == ops[1]) pi = (int)k;
                if (pi < 0) err("unknown parameter '" + ops[1] + "'");
                I.s[0] = mkOp(K_PARAM, params_[pi].second, (uint16_t)pi, 0);
            } else if (parts[1] == "global") {
                I.op = OP_LD_GLOBAL; I.s[0] = parseReg(ops[1]);
            } else err("unknown state space '" + parts[1] + "'");
        }
        else if (m == "st") {
            nparts(3); nops(2);
            if (parts[1] != "global") err("only st.global is supported");
            I.op = OP_ST_GLOBAL; I.type = (uint8_t)needCls(parts[2]);
            I.s[0] = parseReg(ops[0]); I.s[1] = parseOp(ops[1], I.type);
        }
        else if (m == "bra") {
            nparts(1); nops(1);
            I.op = OP_BRA; slot = 0; patchLabel = ops[0];
        }
        else if (m == "call") {
            nparts(2);
            I.op = OP_CALL; I.type = (uint8_t)(parts[1] == "void" ? C_VOID : needCls(parts[1]));
            size_t k = 0;
            if (I.type != C_VOID) { if (ops.empty()) err("call needs a destination"); I.dst = destReg(ops[k++]); }
            if (k >= ops.size()) err("call needs a target");
            int s = symFor(ops[k++]);
            I.s[0] = mkOp(K_SYM, 0, 0, s);
            Reloc r; r.instr = (uint32_t)obj_.text.size(); r.slot = 0; r.sym = s; r.addend = 0;
            obj_.relocs.push_back(r);
            int na = 0;
            for (; k < ops.size(); k++, na++) {
                if (na >= 4) err("call supports at most 4 arguments");
                I.s[1 + na] = parseOp(ops[k], ops[k].rfind("0f", 0) == 0 ? C_F32 : C_S32);
            }
        }
        else if (m == "ret") {
            I.op = OP_RET;
            if (parts.size() == 1) { nops(0); I.type = C_VOID; }
            else { nparts(2); nops(1); I.type = (uint8_t)needCls(parts[1]); I.s[0] = parseOp(ops[0], I.type); }
        }
        else err("unknown instruction '" + mn + "'");

        I.line = curLine_;
        uint32_t pc = (uint32_t)obj_.text.size();
        if (lineDirty_) {
            obj_.lines.push_back({pc, curLine_, (uint32_t)(fileIdx_ < 0 ? 0 : fileIdx_)});
            lineDirty_ = false;
        }
        if (fileIdx_ < 0 && obj_.files.empty()) { obj_.files.push_back("<unknown>"); fileIdx_ = 0; }
        obj_.text.push_back(I);
        if (slot >= 0) patches_.push_back({pc, slot, patchLabel, ln_});
    }
};

}  // namespace

Object assemble(const std::string& text) { return Assembler().run(text); }

}  // namespace mp
