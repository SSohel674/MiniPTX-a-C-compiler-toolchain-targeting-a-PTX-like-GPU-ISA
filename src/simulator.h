// SIMT GPU simulator: warps of lock-step threads with divergence handled by min-PC reconvergence.
#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>
#include "common.h"
#include "object.h"

namespace mp {

struct Frame {
    uint32_t func = 0, pc = 0;
    std::vector<uint64_t> r[4];  // register banks: s32, f32, pred, u64
    uint64_t args[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    Operand callDst;             // where the caller wants the return value
    uint8_t callType = C_VOID;
};

struct Thread {
    std::vector<Frame> st;
    bool done = false;
    uint32_t tid = 0;  // thread index within the block
};

class Machine {
public:
    struct Peek { uint32_t block = 0, warp = 0, pc = 0, mask = 0; };
    struct Stats { uint64_t warpInstrs = 0, laneInstrs = 0, divergent = 0, calls = 0; };

    explicit Machine(const Object& prog, uint32_t warpSize = 32);

    // device memory (flat byte-addressed; address 0 is never valid)
    uint64_t alloc(size_t bytes);
    void write(uint64_t addr, const void* src, size_t n);
    void read(uint64_t addr, void* dst, size_t n) const;

    void launch(int kernelSym, uint32_t grid, uint32_t block, const std::vector<uint64_t>& args);
    bool step();   // execute one warp instruction; false once the grid has finished
    void run();
    bool finished() const { return finished_; }
    bool launched() const { return launched_; }
    Peek peek() const;                 // next warp instruction to run (valid while !finished())
    uint32_t laneCount() const;        // lanes in the current warp
    Thread* lane(uint32_t i);          // nullptr if out of range
    const Stats& stats() const { return stats_; }
    const Object& program() const { return prog_; }
    uint32_t warpSize() const { return ws_; }
    uint32_t blockDim() const { return blockDim_; }
    uint32_t gridDim() const { return gridDim_; }
    void setStepLimit(uint64_t n) { limit_ = n; }

private:
    const Object& prog_;
    uint32_t ws_;
    std::vector<uint8_t> mem_;
    std::unordered_map<uint32_t, int> funcAt_;
    int kernel_ = -1;
    std::vector<uint64_t> kargs_;
    uint32_t gridDim_ = 0, blockDim_ = 0, block_ = 0, warp_ = 0, nWarps_ = 0;
    std::vector<Thread> threads_;
    bool finished_ = true, launched_ = false;
    Stats stats_;
    uint64_t limit_ = 400000000ULL;

    void startBlock(uint32_t b);
    bool warpAlive() const;
    void settle();
    void exec(Thread& t, const Instr& in);
    uint64_t rd(Thread& t, const Operand& o) const;
    void wr(Frame& f, const Operand& o, uint64_t v);
    Frame makeFrame(int sym) const;
};

}  // namespace mp
