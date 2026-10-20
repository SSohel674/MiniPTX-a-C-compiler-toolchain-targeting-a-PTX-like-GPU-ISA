// MiniPTX virtual ISA: register classes, opcodes, and the fixed-width binary instruction.
#pragma once
#include <climits>
#include <cstdint>
#include <cstring>

namespace mp {

// Register classes double as value types: %r = s32, %f = f32, %p = pred, %rd = u64 (addresses)
enum Cls : uint8_t { C_S32 = 0, C_F32 = 1, C_PRED = 2, C_U64 = 3, C_VOID = 255 };

inline const char* clsName(int c) {
    static const char* n[] = {"s32", "f32", "pred", "u64"};
    return (c >= 0 && c < 4) ? n[c] : "void";
}
inline const char* regPrefix(int c) {
    static const char* n[] = {"r", "f", "p", "rd"};
    return n[c & 3];
}

enum Op : uint8_t {
    OP_NOP = 0, OP_MOV, OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_REM, OP_NEG,
    OP_AND, OP_OR, OP_NOT, OP_SETP, OP_CVT, OP_MUL_WIDE,
    OP_LD_PARAM, OP_LD_GLOBAL, OP_ST_GLOBAL,
    OP_BRA, OP_CALL, OP_RET, OP_COUNT
};
enum Cmp : uint8_t { CMP_LT = 0, CMP_LE, CMP_GT, CMP_GE, CMP_EQ, CMP_NE };
enum Kind : uint8_t { K_NONE = 0, K_REG, K_IMM, K_LABEL, K_SYM, K_PARAM, K_SPECIAL };
enum Special : int32_t { SP_TID = 0, SP_CTAID, SP_NTID, SP_NCTAID };

#pragma pack(push, 1)
struct Operand {
    uint8_t kind = K_NONE;
    uint8_t cls = 0;
    uint16_t idx = 0;   // register index / param index
    int32_t imm = 0;    // immediate bits, code address, symbol index or special id
};
// 64-byte fixed-width instruction: [op,type,cmp,predmode] pred dst s[5] line
struct Instr {
    uint8_t op = OP_NOP, type = 0, cmp = 0, predmode = 0;  // predmode: 0 none, 1 @p, 2 @!p
    Operand pred, dst, s[5];
    uint32_t line = 0;
};
#pragma pack(pop)
static_assert(sizeof(Operand) == 8, "Operand must be 8 bytes");
static_assert(sizeof(Instr) == 64, "Instr must be 64 bytes");

inline float bitsToF(int32_t b) { float f; std::memcpy(&f, &b, 4); return f; }
inline int32_t fToBits(float f) { int32_t b; std::memcpy(&b, &f, 4); return b; }
inline int32_t cvtF2I(float f) {
    if (f != f) return 0;
    if (f >= 2147483648.0f) return INT_MAX;
    if (f <= -2147483648.0f) return INT_MIN;
    return (int32_t)f;
}

inline Operand mkOp(uint8_t kind, uint8_t cls, uint16_t idx, int32_t imm) {
    Operand o; o.kind = kind; o.cls = cls; o.idx = idx; o.imm = imm; return o;
}
inline Operand mkReg(int idx, uint8_t cls) { return mkOp(K_REG, cls, (uint16_t)idx, 0); }
inline Operand mkImm(int32_t v, uint8_t cls) { return mkOp(K_IMM, cls, 0, v); }

}  // namespace mp
