// Mid-level IR: the same 64-byte Instr as the ISA, but over unlimited virtual registers
// (mutable, non-SSA) plus an IR_LABEL pseudo-op. Branch labels are K_LABEL with imm = label id.
#pragma once
#include <string>
#include <utility>
#include <vector>
#include "isa.h"

namespace mp {

constexpr uint8_t IR_LABEL = 200;

struct IRFunc {
    std::string name;
    bool kernel = false;
    uint8_t retCls = C_VOID;
    std::vector<std::pair<std::string, uint8_t>> params;  // name, class
    std::vector<Instr> code;
    std::vector<std::string> callees;  // K_SYM operands index into this table
    std::vector<uint8_t> vcls;         // class of each virtual register
    std::vector<std::string> vname;    // user variable name ("" for temporaries)
    int nlabels = 0;
    int line = 0;
};

struct ExternDecl {
    std::string name;
    uint8_t retCls = C_VOID;
    std::vector<std::pair<std::string, uint8_t>> params;
};

struct IRModule {
    std::string file;
    std::vector<IRFunc> funcs;
    std::vector<ExternDecl> externs;
};

}  // namespace mp
