#include "simulator.h"
#include <algorithm>
#include <cstdio>
#include <string>

namespace mp {
namespace {
inline int32_t asI(uint64_t v) { return (int32_t)(uint32_t)v; }
inline float asF(uint64_t v) { return bitsToF((int32_t)(uint32_t)v); }
inline uint64_t fromI(int32_t v) { return (uint64_t)(uint32_t)v; }
inline uint64_t fromF(float f) { return (uint64_t)(uint32_t)fToBits(f); }
std::string hex(uint64_t v) { char b[32]; std::snprintf(b, sizeof b, "0x%llx", (unsigned long long)v); return b; }
}  // namespace

Machine::Machine(const Object& prog, uint32_t warpSize) : prog_(prog), ws_(warpSize ? warpSize : 32) {
    mem_.assign(256, 0);  // reserve the low page so 0 is a null pointer
    for (size_t i = 0; i < prog_.syms.size(); i++)
        if (prog_.syms[i].kind != SYM_UNDEF) funcAt_[prog_.syms[i].value] = (int)i;
}

uint64_t Machine::alloc(size_t bytes) {
    size_t addr = (mem_.size() + 15) & ~size_t(15);
    mem_.resize(addr + std::max<size_t>(bytes, 4), 0);
    return addr;
}

void Machine::write(uint64_t addr, const void* src, size_t n) {
    if (addr < 256 || addr + n > mem_.size()) throw SimError("illegal memory write at " + hex(addr));
    std::memcpy(mem_.data() + addr, src, n);
}
void Machine::read(uint64_t addr, void* dst, size_t n) const {
    if (addr < 256 || addr + n > mem_.size()) throw SimError("illegal memory read at " + hex(addr));
    std::memcpy(dst, mem_.data() + addr, n);
}

Frame Machine::makeFrame(int sym) const {
    Frame f;
    f.func = (uint32_t)sym;
    f.pc = prog_.syms[sym].value;
    for (int c = 0; c < 4; c++) f.r[c].assign(prog_.syms[sym].nregs[c], 0);
    return f;
}

void Machine::launch(int kernelSym, uint32_t grid, uint32_t block, const std::vector<uint64_t>& args) {
    if (kernelSym < 0 || (size_t)kernelSym >= prog_.syms.size() || prog_.syms[kernelSym].kind != SYM_KERNEL)
        throw SimError("launch: not a kernel");
    if (grid == 0 || block == 0) throw SimError("launch: grid and block must be > 0");
    if (args.size() != prog_.syms[kernelSym].nparams)
        throw SimError("launch: kernel '" + prog_.syms[kernelSym].name + "' expects " +
                       std::to_string(prog_.syms[kernelSym].nparams) + " argument(s), got " + std::to_string(args.size()));
    kernel_ = kernelSym; kargs_ = args; gridDim_ = grid; blockDim_ = block;
    nWarps_ = (block + ws_ - 1) / ws_;
    launched_ = true; finished_ = false; stats_ = Stats();
    startBlock(0);
    settle();
}

void Machine::startBlock(uint32_t b) {
    block_ = b; warp_ = 0;
    threads_.assign(blockDim_, Thread());
    for (uint32_t t = 0; t < blockDim_; t++) {
        threads_[t].tid = t;
        Frame f = makeFrame(kernel_);
        for (size_t i = 0; i < kargs_.size() && i < 8; i++) f.args[i] = kargs_[i];
        threads_[t].st.push_back(std::move(f));
    }
}

uint32_t Machine::laneCount() const {
    uint32_t lo = warp_ * ws_;
    return std::min(blockDim_, lo + ws_) - lo;
}
Thread* Machine::lane(uint32_t i) { return i < laneCount() ? &threads_[warp_ * ws_ + i] : nullptr; }

bool Machine::warpAlive() const {
    uint32_t lo = warp_ * ws_, hi = std::min(blockDim_, lo + ws_);
    for (uint32_t t = lo; t < hi; t++) if (!threads_[t].done) return true;
    return false;
}

void Machine::settle() {
    while (!finished_ && !warpAlive()) {
        if (++warp_ >= nWarps_) {
            if (++block_ >= gridDim_) { finished_ = true; return; }
            startBlock(block_);
        }
    }
}

Machine::Peek Machine::peek() const {
    Peek p; p.block = block_; p.warp = warp_;
    uint32_t lo = warp_ * ws_, hi = std::min(blockDim_, lo + ws_);
    uint32_t best = UINT32_MAX;
    for (uint32_t t = lo; t < hi; t++) if (!threads_[t].done) best = std::min(best, threads_[t].st.back().pc);
    p.pc = best;
    for (uint32_t t = lo; t < hi; t++)
        if (!threads_[t].done && threads_[t].st.back().pc == best) p.mask |= 1u << (t - lo);
    return p;
}

bool Machine::step() {
    if (finished_) return false;
    if (++stats_.warpInstrs > limit_) throw SimError("execution limit exceeded (infinite loop?)");
    Peek pk = peek();
    if (pk.pc >= prog_.text.size()) throw SimError("execution fell off the end of the program at pc " + std::to_string(pk.pc));
    const Instr in = prog_.text[pk.pc];
    uint32_t lo = warp_ * ws_, hi = std::min(blockDim_, lo + ws_);
    uint32_t active = 0, alive = 0;
    for (uint32_t t = lo; t < hi; t++) {
        Thread& th = threads_[t];
        if (th.done) continue;
        alive++;
        if (th.st.back().pc != pk.pc) continue;
        active++;
        try {
            exec(th, in);
        } catch (SimError& e) {
            throw SimError(std::string(e.what()) + " [block " + std::to_string(block_) + ", thread " + std::to_string(th.tid) +
                           ", pc " + std::to_string(pk.pc) + ", source line " + std::to_string(in.line) + "]");
        }
    }
    stats_.laneInstrs += active;
    if (active < alive) stats_.divergent++;
    settle();
    return !finished_;
}

void Machine::run() { while (step()) {} }

uint64_t Machine::rd(Thread& t, const Operand& o) const {
    Frame& f = t.st.back();
    switch (o.kind) {
    case K_REG:
        if (o.idx >= f.r[o.cls & 3].size()) throw SimError("register index out of range");
        return f.r[o.cls & 3][o.idx];
    case K_IMM:
        if (o.cls == C_U64) return (uint64_t)(int64_t)o.imm;
        if (o.cls == C_PRED) return o.imm != 0;
        return (uint64_t)(uint32_t)o.imm;
    case K_SPECIAL:
        switch (o.imm) {
        case SP_TID: return t.tid;
        case SP_CTAID: return block_;
        case SP_NTID: return blockDim_;
        default: return gridDim_;
        }
    case K_PARAM: return f.args[o.idx & 7];
    default: throw SimError("bad source operand");
    }
}

void Machine::wr(Frame& f, const Operand& o, uint64_t v) {
    if (o.kind != K_REG || o.idx >= f.r[o.cls & 3].size()) throw SimError("bad destination register");
    f.r[o.cls & 3][o.idx] = v;
}

void Machine::exec(Thread& t, const Instr& in) {
    if (in.predmode) {
        bool p = rd(t, in.pred) != 0;
        if (in.predmode == 2) p = !p;
        if (!p) { t.st.back().pc++; return; }
    }
    auto S = [&](int k) { return rd(t, in.s[k]); };
    Frame& f = t.st.back();
    switch (in.op) {
    case OP_NOP: break;
    case OP_MOV: wr(f, in.dst, S(0)); break;
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_REM: {
        uint64_t a = S(0), b = S(1), r = 0;
        if (in.type == C_S32) {
            int32_t x = asI(a), y = asI(b);
            uint32_t ux = (uint32_t)x, uy = (uint32_t)y;
            switch (in.op) {
            case OP_ADD: r = fromI((int32_t)(ux + uy)); break;
            case OP_SUB: r = fromI((int32_t)(ux - uy)); break;
            case OP_MUL: r = fromI((int32_t)(ux * uy)); break;
            case OP_DIV:
                if (y == 0) throw SimError("integer divide by zero");
                r = fromI((x == INT_MIN && y == -1) ? INT_MIN : x / y); break;
            default:
                if (y == 0) throw SimError("integer remainder by zero");
                r = fromI((x == INT_MIN && y == -1) ? 0 : x % y); break;
            }
        } else if (in.type == C_F32) {
            float x = asF(a), y = asF(b), z = 0;
            switch (in.op) {
            case OP_ADD: z = x + y; break;
            case OP_SUB: z = x - y; break;
            case OP_MUL: z = x * y; break;
            case OP_DIV: z = x / y; break;
            default: throw SimError("rem is not defined for f32");
            }
            r = fromF(z);
        } else if (in.type == C_U64) {
            switch (in.op) {
            case OP_ADD: r = a + b; break;
            case OP_SUB: r = a - b; break;
            case OP_MUL: r = a * b; break;
            default: throw SimError("unsupported u64 operation");
            }
        } else throw SimError("bad arithmetic type");
        wr(f, in.dst, r);
        break;
    }
    case OP_NEG:
        if (in.type == C_F32) wr(f, in.dst, fromF(-asF(S(0))));
        else wr(f, in.dst, fromI((int32_t)(0u - (uint32_t)asI(S(0)))));
        break;
    case OP_AND: wr(f, in.dst, (S(0) != 0) && (S(1) != 0)); break;
    case OP_OR: wr(f, in.dst, (S(0) != 0) || (S(1) != 0)); break;
    case OP_NOT: wr(f, in.dst, S(0) == 0); break;
    case OP_SETP: {
        bool r;
        if (in.type == C_F32) {
            float x = asF(S(0)), y = asF(S(1));
            switch (in.cmp) { case CMP_LT: r = x < y; break; case CMP_LE: r = x <= y; break; case CMP_GT: r = x > y; break;
                              case CMP_GE: r = x >= y; break; case CMP_EQ: r = x == y; break; default: r = x != y; }
        } else {
            int32_t x = asI(S(0)), y = asI(S(1));
            switch (in.cmp) { case CMP_LT: r = x < y; break; case CMP_LE: r = x <= y; break; case CMP_GT: r = x > y; break;
                              case CMP_GE: r = x >= y; break; case CMP_EQ: r = x == y; break; default: r = x != y; }
        }
        wr(f, in.dst, r);
        break;
    }
    case OP_CVT: {
        uint64_t a = S(0);
        if (in.type == C_F32 && in.cmp == C_S32) wr(f, in.dst, fromF((float)asI(a)));
        else if (in.type == C_S32 && in.cmp == C_F32) wr(f, in.dst, fromI(cvtF2I(asF(a))));
        else if (in.type == C_U64 && in.cmp == C_S32) wr(f, in.dst, (uint64_t)(int64_t)asI(a));
        else if (in.type == C_S32 && in.cmp == C_U64) wr(f, in.dst, fromI((int32_t)a));
        else if (in.type == in.cmp) wr(f, in.dst, a);
        else throw SimError("unsupported cvt");
        break;
    }
    case OP_MUL_WIDE: wr(f, in.dst, (uint64_t)((int64_t)asI(S(0)) * (int64_t)asI(S(1)))); break;
    case OP_LD_PARAM: wr(f, in.dst, S(0)); break;
    case OP_LD_GLOBAL: {
        uint64_t addr = S(0);
        if (addr % 4) throw SimError("misaligned load at " + hex(addr));
        uint32_t v;
        read(addr, &v, 4);
        wr(f, in.dst, (uint64_t)v);
        break;
    }
    case OP_ST_GLOBAL: {
        uint64_t addr = S(0);
        if (addr % 4) throw SimError("misaligned store at " + hex(addr));
        uint32_t v = (uint32_t)S(1);
        write(addr, &v, 4);
        break;
    }
    case OP_BRA: f.pc = (uint32_t)in.s[0].imm; return;
    case OP_CALL: {
        auto it = funcAt_.find((uint32_t)in.s[0].imm);
        if (it == funcAt_.end()) throw SimError("call to unresolved address " + std::to_string(in.s[0].imm));
        if (t.st.size() >= 128) throw SimError("call stack overflow (recursion too deep)");
        Frame callee = makeFrame(it->second);
        for (int k = 0; k < 4 && in.s[1 + k].kind != K_NONE; k++) callee.args[k] = S(1 + k);
        callee.callDst = in.dst; callee.callType = in.type;
        f.pc++;
        stats_.calls++;
        t.st.push_back(std::move(callee));
        return;
    }
    case OP_RET: {
        uint64_t v = in.type != C_VOID ? S(0) : 0;
        Frame done = std::move(t.st.back());
        t.st.pop_back();
        if (t.st.empty()) { t.done = true; return; }
        if (done.callType != C_VOID) wr(t.st.back(), done.callDst, v);
        return;
    }
    default: throw SimError("illegal instruction");
    }
    f.pc++;
}

}  // namespace mp
