// Optimizer passes over the virtual-register IR:
//   1. block-local constant/copy propagation + constant folding + algebraic simplification
//   2. block-local common subexpression elimination (value numbering, load-aware)
//   3. unreachable-code / redundant-branch / unused-label cleanup (CFG)
//   4. global dead-code elimination (backward liveness dataflow over basic blocks)
#include "opt.h"
#include <algorithm>
#include <climits>
#include <cstring>
#include <map>
#include <string>

namespace mp {
namespace {

bool definesDst(const Instr& i) {
    switch (i.op) {
    case OP_MOV: case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_REM: case OP_NEG: case OP_AND:
    case OP_OR: case OP_NOT: case OP_SETP: case OP_CVT: case OP_MUL_WIDE: case OP_LD_PARAM: case OP_LD_GLOBAL:
        return true;
    case OP_CALL: return i.type != C_VOID;
    default: return false;
    }
}
bool removableIfDead(const Instr& i) { return definesDst(i) && i.op != OP_CALL; }

template <class F> void forEachUse(Instr& i, F f) {
    for (auto& s : i.s) if (s.kind == K_REG) f(s);
    if (i.predmode && i.pred.kind == K_REG) f(i.pred);
}
template <class F> void forEachUseC(const Instr& i, F f) {
    for (auto& s : i.s) if (s.kind == K_REG) f(s);
    if (i.predmode && i.pred.kind == K_REG) f(i.pred);
}

void toMov(Instr& i, Operand src) {
    uint8_t c = i.dst.cls;
    i.op = OP_MOV; i.type = c; i.cmp = 0;
    for (auto& s : i.s) s = Operand();
    i.s[0] = src;
}

bool fold(const Instr& i, Operand& out) {
    auto imm = [&](int k) { return i.s[k].kind == K_IMM; };
    switch (i.op) {
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_REM: {
        if (!imm(0) || !imm(1)) return false;
        if (i.type == C_S32) {
            int32_t a = i.s[0].imm, b = i.s[1].imm, r = 0;
            uint32_t ua = (uint32_t)a, ub = (uint32_t)b;
            switch (i.op) {
            case OP_ADD: r = (int32_t)(ua + ub); break;
            case OP_SUB: r = (int32_t)(ua - ub); break;
            case OP_MUL: r = (int32_t)(ua * ub); break;
            case OP_DIV: if (b == 0 || (a == INT_MIN && b == -1)) return false; r = a / b; break;
            default: if (b == 0 || (a == INT_MIN && b == -1)) return false; r = a % b; break;
            }
            out = mkImm(r, C_S32);
            return true;
        }
        if (i.type == C_F32) {
            float a = bitsToF(i.s[0].imm), b = bitsToF(i.s[1].imm), r = 0;
            switch (i.op) {
            case OP_ADD: r = a + b; break;
            case OP_SUB: r = a - b; break;
            case OP_MUL: r = a * b; break;
            case OP_DIV: if (b == 0) return false; r = a / b; break;
            default: return false;
            }
            out = mkImm(fToBits(r), C_F32);
            return true;
        }
        if (i.type == C_U64 && i.op != OP_DIV && i.op != OP_REM) {
            int64_t a = i.s[0].imm, b = i.s[1].imm;
            int64_t r = i.op == OP_ADD ? a + b : i.op == OP_SUB ? a - b : a * b;
            if (r < INT_MIN || r > INT_MAX) return false;
            out = mkImm((int32_t)r, C_U64);
            return true;
        }
        return false;
    }
    case OP_NEG:
        if (!imm(0)) return false;
        if (i.type == C_S32) { out = mkImm((int32_t)(0u - (uint32_t)i.s[0].imm), C_S32); return true; }
        if (i.type == C_F32) { out = mkImm(fToBits(-bitsToF(i.s[0].imm)), C_F32); return true; }
        return false;
    case OP_NOT: if (!imm(0)) return false; out = mkImm(i.s[0].imm == 0, C_PRED); return true;
    case OP_AND: if (!imm(0) || !imm(1)) return false; out = mkImm((i.s[0].imm != 0) && (i.s[1].imm != 0), C_PRED); return true;
    case OP_OR: if (!imm(0) || !imm(1)) return false; out = mkImm((i.s[0].imm != 0) || (i.s[1].imm != 0), C_PRED); return true;
    case OP_SETP: {
        if (!imm(0) || !imm(1)) return false;
        bool r;
        if (i.type == C_S32) {
            int32_t a = i.s[0].imm, b = i.s[1].imm;
            switch (i.cmp) { case CMP_LT: r = a < b; break; case CMP_LE: r = a <= b; break; case CMP_GT: r = a > b; break;
                             case CMP_GE: r = a >= b; break; case CMP_EQ: r = a == b; break; default: r = a != b; }
        } else if (i.type == C_F32) {
            float a = bitsToF(i.s[0].imm), b = bitsToF(i.s[1].imm);
            switch (i.cmp) { case CMP_LT: r = a < b; break; case CMP_LE: r = a <= b; break; case CMP_GT: r = a > b; break;
                             case CMP_GE: r = a >= b; break; case CMP_EQ: r = a == b; break; default: r = a != b; }
        } else return false;
        out = mkImm(r, C_PRED);
        return true;
    }
    case OP_CVT: {
        if (!imm(0)) return false;
        if (i.type == C_F32 && i.cmp == C_S32) { out = mkImm(fToBits((float)i.s[0].imm), C_F32); return true; }
        if (i.type == C_S32 && i.cmp == C_F32) { out = mkImm(cvtF2I(bitsToF(i.s[0].imm)), C_S32); return true; }
        return false;
    }
    case OP_MUL_WIDE: {
        if (!imm(0) || !imm(1)) return false;
        int64_t r = (int64_t)i.s[0].imm * (int64_t)i.s[1].imm;
        if (r < INT_MIN || r > INT_MAX) return false;
        out = mkImm((int32_t)r, C_U64);
        return true;
    }
    default: return false;
    }
}

// ---------------------------------------------------------------- pass 1
bool localSimplify(IRFunc& f, OptStats& st) {
    bool changed = false;
    std::map<int, Operand> cst;
    std::map<int, int> copy;
    auto reset = [&] { cst.clear(); copy.clear(); };
    auto kill = [&](int v) {
        cst.erase(v); copy.erase(v);
        for (auto it = copy.begin(); it != copy.end();) it = (it->second == v) ? copy.erase(it) : std::next(it);
    };
    for (auto& in : f.code) {
        if (in.op == IR_LABEL) { reset(); continue; }
        if (in.op == OP_NOP) continue;
        // propagate into sources
        for (int k = 0; k < 5; k++) {
            Operand& s = in.s[k];
            if (s.kind != K_REG) continue;
            bool addrOperand = (in.op == OP_LD_GLOBAL && k == 0) || (in.op == OP_ST_GLOBAL && k == 0);
            auto c = cst.find(s.idx);
            if (c != cst.end() && !addrOperand) { s = c->second; changed = true; continue; }
            auto cp = copy.find(s.idx);
            if (cp != copy.end()) { s = mkReg(cp->second, s.cls); changed = true; }
        }
        if (in.predmode && in.pred.kind == K_REG) {
            auto c = cst.find(in.pred.idx);
            if (c != cst.end()) {
                bool t = c->second.imm != 0;
                if (in.predmode == 2) t = !t;
                if (t) { in.predmode = 0; in.pred = Operand(); } else in.op = OP_NOP;
                st.folded++; changed = true;
                if (in.op == OP_NOP) continue;
            } else {
                auto cp = copy.find(in.pred.idx);
                if (cp != copy.end()) { in.pred = mkReg(cp->second, in.pred.cls); changed = true; }
            }
        }
        // fold / simplify
        if (definesDst(in) && in.op != OP_MOV && in.op != OP_CALL && in.op != OP_LD_PARAM && in.op != OP_LD_GLOBAL) {
            Operand out;
            if (fold(in, out)) { toMov(in, out); st.folded++; changed = true; }
            else {
                auto isI = [&](int k, int32_t v) { return in.s[k].kind == K_IMM && in.s[k].imm == v; };
                auto isReg = [&](int k) { return in.s[k].kind == K_REG; };
                const int32_t ONE = fToBits(1.0f);
                if (in.type == C_S32) {
                    if (in.op == OP_ADD && isI(1, 0) && isReg(0)) { toMov(in, in.s[0]); st.folded++; changed = true; }
                    else if (in.op == OP_ADD && isI(0, 0) && isReg(1)) { toMov(in, in.s[1]); st.folded++; changed = true; }
                    else if (in.op == OP_SUB && isI(1, 0) && isReg(0)) { toMov(in, in.s[0]); st.folded++; changed = true; }
                    else if (in.op == OP_MUL && isI(1, 1) && isReg(0)) { toMov(in, in.s[0]); st.folded++; changed = true; }
                    else if (in.op == OP_MUL && isI(0, 1) && isReg(1)) { toMov(in, in.s[1]); st.folded++; changed = true; }
                    else if (in.op == OP_MUL && (isI(0, 0) || isI(1, 0))) { toMov(in, mkImm(0, C_S32)); st.folded++; changed = true; }
                    else if (in.op == OP_DIV && isI(1, 1) && isReg(0)) { toMov(in, in.s[0]); st.folded++; changed = true; }
                } else if (in.type == C_F32) {
                    if (in.op == OP_MUL && in.s[1].kind == K_IMM && in.s[1].imm == ONE && isReg(0)) { toMov(in, in.s[0]); st.folded++; changed = true; }
                    else if (in.op == OP_MUL && in.s[0].kind == K_IMM && in.s[0].imm == ONE && isReg(1)) { toMov(in, in.s[1]); st.folded++; changed = true; }
                    else if (in.op == OP_DIV && in.s[1].kind == K_IMM && in.s[1].imm == ONE && isReg(0)) { toMov(in, in.s[0]); st.folded++; changed = true; }
                }
            }
        }
        if (definesDst(in)) {
            int d = in.dst.idx;
            kill(d);
            if (in.op == OP_MOV) {
                if (in.s[0].kind == K_IMM) cst[d] = in.s[0];
                else if (in.s[0].kind == K_REG && (int)in.s[0].idx != d) copy[d] = in.s[0].idx;
            }
        }
    }
    size_t before = f.code.size();
    f.code.erase(std::remove_if(f.code.begin(), f.code.end(), [](const Instr& i) { return i.op == OP_NOP; }), f.code.end());
    return changed || before != f.code.size();
}

// ---------------------------------------------------------------- pass 2
struct Key { uint8_t op, type, cmp; Operand s[5]; };
bool cseCandidate(const Instr& i) {
    switch (i.op) {
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_REM: case OP_NEG: case OP_AND: case OP_OR:
    case OP_NOT: case OP_SETP: case OP_CVT: case OP_MUL_WIDE: case OP_LD_GLOBAL: return true;
    case OP_MOV: return i.s[0].kind == K_SPECIAL;
    default: return false;
    }
}

bool localCSE(IRFunc& f, OptStats& st) {
    struct Entry { std::string key; int result; Operand srcs[5]; bool isLoad; };
    std::vector<Entry> table;
    bool changed = false;
    auto invalidate = [&](int v) {
        table.erase(std::remove_if(table.begin(), table.end(), [&](const Entry& e) {
            if (e.result == v) return true;
            for (auto& s : e.srcs) if (s.kind == K_REG && s.idx == v) return true;
            return false;
        }), table.end());
    };
    for (auto& in : f.code) {
        if (in.op == IR_LABEL) { table.clear(); continue; }
        if (in.op == OP_ST_GLOBAL || in.op == OP_CALL) {
            table.erase(std::remove_if(table.begin(), table.end(), [](const Entry& e) { return e.isLoad; }), table.end());
        }
        if (!definesDst(in)) continue;
        int d = in.dst.idx;
        if (cseCandidate(in) && !in.predmode) {
            Key k{};
            k.op = in.op; k.type = in.type; k.cmp = in.cmp;
            for (int j = 0; j < 5; j++) k.s[j] = in.s[j];
            std::string key(reinterpret_cast<const char*>(&k), sizeof k);
            const Entry* hit = nullptr;
            for (auto& e : table) if (e.key == key) { hit = &e; break; }
            if (hit && hit->result != d) {
                toMov(in, mkReg(hit->result, in.dst.cls));
                st.cse++; changed = true;
                invalidate(d);
                continue;
            }
            invalidate(d);
            bool selfRef = false;
            for (auto& s : in.s) if (s.kind == K_REG && s.idx == d) selfRef = true;
            if (!selfRef) {
                Entry e; e.key = key; e.result = d; e.isLoad = in.op == OP_LD_GLOBAL;
                for (int j = 0; j < 5; j++) e.srcs[j] = in.s[j];
                table.push_back(e);
            }
        } else invalidate(d);
    }
    return changed;
}

// ---------------------------------------------------------------- CFG helpers
struct Block { int start, end; std::vector<int> succ; };  // [start, end)

std::vector<Block> buildBlocks(const IRFunc& f) {
    std::vector<Block> bl;
    int n = (int)f.code.size();
    std::vector<char> leader(n + 1, 0);
    if (n) leader[0] = 1;
    for (int i = 0; i < n; i++) {
        if (f.code[i].op == IR_LABEL) leader[i] = 1;
        if (f.code[i].op == OP_BRA || f.code[i].op == OP_RET) leader[i + 1] = 1;
    }
    for (int i = 0; i < n;) {
        int j = i + 1;
        while (j < n && !leader[j]) j++;
        bl.push_back({i, j, {}});
        i = j;
    }
    std::map<int, int> labelBlock;
    for (size_t b = 0; b < bl.size(); b++)
        for (int i = bl[b].start; i < bl[b].end; i++)
            if (f.code[i].op == IR_LABEL) labelBlock[f.code[i].s[0].imm] = (int)b;
    for (size_t b = 0; b < bl.size(); b++) {
        const Instr& last = f.code[bl[b].end - 1];
        bool fall = true;
        if (last.op == OP_BRA) {
            auto it = labelBlock.find(last.s[0].imm);
            if (it != labelBlock.end()) bl[b].succ.push_back(it->second);
            if (!last.predmode) fall = false;
        } else if (last.op == OP_RET) fall = false;
        if (fall && b + 1 < bl.size()) bl[b].succ.push_back((int)b + 1);
    }
    return bl;
}

// ---------------------------------------------------------------- pass 3
bool simplifyCFG(IRFunc& f, OptStats& st) {
    bool changed = false;
    if (f.code.empty()) return false;
    {   // remove unreachable blocks
        auto bl = buildBlocks(f);
        std::vector<char> reach(bl.size(), 0);
        std::vector<int> work{0};
        reach[0] = 1;
        while (!work.empty()) {
            int b = work.back(); work.pop_back();
            for (int s : bl[b].succ) if (!reach[s]) { reach[s] = 1; work.push_back(s); }
        }
        std::vector<Instr> out;
        for (size_t b = 0; b < bl.size(); b++) {
            if (reach[b]) out.insert(out.end(), f.code.begin() + bl[b].start, f.code.begin() + bl[b].end);
            else { st.cfg += bl[b].end - bl[b].start; changed = true; }
        }
        f.code.swap(out);
    }
    {   // branch to the immediately following label is a no-op
        std::vector<Instr> out;
        for (size_t i = 0; i < f.code.size(); i++) {
            const Instr& in = f.code[i];
            if (in.op == OP_BRA) {
                bool toNext = false;
                for (size_t j = i + 1; j < f.code.size() && f.code[j].op == IR_LABEL; j++)
                    if (f.code[j].s[0].imm == in.s[0].imm) { toNext = true; break; }
                if (toNext) { st.cfg++; changed = true; continue; }
            }
            out.push_back(in);
        }
        f.code.swap(out);
    }
    {   // drop labels nobody jumps to
        std::vector<char> used(f.nlabels + 1, 0);
        for (auto& in : f.code) if (in.op == OP_BRA) used[in.s[0].imm] = 1;
        std::vector<Instr> out;
        for (auto& in : f.code) {
            if (in.op == IR_LABEL && !used[in.s[0].imm]) { st.cfg++; changed = true; continue; }
            out.push_back(in);
        }
        f.code.swap(out);
    }
    return changed;
}

// ---------------------------------------------------------------- pass 4
bool dce(IRFunc& f, OptStats& st) {
    if (f.code.empty()) return false;
    auto bl = buildBlocks(f);
    size_t V = f.vcls.size();
    std::vector<std::vector<char>> liveIn(bl.size(), std::vector<char>(V, 0)), liveOut = liveIn;
    bool again = true;
    while (again) {
        again = false;
        for (int b = (int)bl.size() - 1; b >= 0; b--) {
            std::vector<char> out(V, 0);
            for (int s : bl[b].succ) for (size_t v = 0; v < V; v++) if (liveIn[s][v]) out[v] = 1;
            std::vector<char> live = out;
            for (int i = bl[b].end - 1; i >= bl[b].start; i--) {
                const Instr& in = f.code[i];
                if (definesDst(in) && !in.predmode) live[in.dst.idx] = 0;
                forEachUseC(in, [&](const Operand& o) { live[o.idx] = 1; });
            }
            if (out != liveOut[b] || live != liveIn[b]) { liveOut[b] = out; liveIn[b] = live; again = true; }
        }
    }
    std::vector<char> dead(f.code.size(), 0);
    bool changed = false;
    for (size_t b = 0; b < bl.size(); b++) {
        std::vector<char> live = liveOut[b];
        for (int i = bl[b].end - 1; i >= bl[b].start; i--) {
            const Instr& in = f.code[i];
            if (removableIfDead(in) && !live[in.dst.idx]) { dead[i] = 1; st.dce++; changed = true; continue; }
            if (definesDst(in) && !in.predmode) live[in.dst.idx] = 0;
            forEachUseC(in, [&](const Operand& o) { live[o.idx] = 1; });
        }
    }
    if (changed) {
        std::vector<Instr> out;
        for (size_t i = 0; i < f.code.size(); i++) if (!dead[i]) out.push_back(f.code[i]);
        f.code.swap(out);
    }
    return changed;
}

}  // namespace

void optimize(IRFunc& f, OptStats& st) {
    st.before += f.code.size();
    for (int iter = 0; iter < 8; iter++) {
        bool c = false;
        c |= localSimplify(f, st);
        c |= localCSE(f, st);
        c |= simplifyCFG(f, st);
        c |= dce(f, st);
        if (!c) break;
    }
    st.after += f.code.size();
}

}  // namespace mp
