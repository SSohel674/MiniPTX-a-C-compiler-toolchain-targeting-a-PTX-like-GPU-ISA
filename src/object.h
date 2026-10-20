// Relocatable object / executable image: data model, binary (de)serialization, disassembler.
#pragma once
#include <functional>
#include <string>
#include <vector>
#include "isa.h"

namespace mp {

enum SymKind : uint8_t { SYM_KERNEL = 0, SYM_DEVICE = 1, SYM_UNDEF = 2 };

struct Symbol {
    std::string name;
    uint32_t value = 0, size = 0;  // entry instruction index, instruction count
    uint8_t kind = SYM_UNDEF, nparams = 0, retCls = C_VOID;
    uint8_t paramCls[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    uint16_t nregs[4] = {0, 0, 0, 0};
};

// sym < 0  : section-relative (add the module's .text base); otherwise absolute address of symbol
struct Reloc { uint32_t instr = 0; uint8_t slot = 0; int32_t sym = -1, addend = 0; };
struct LineEnt { uint32_t pc = 0, line = 0, file = 0; };
struct VarEnt { uint32_t sym = 0; uint8_t cls = 0; uint16_t idx = 0; std::string name; uint32_t lo = 0, hi = 0; };

struct Object {
    bool exec = false;  // false: relocatable (.mpo), true: linked executable (.mpx)
    std::vector<Instr> text;
    std::vector<Symbol> syms;
    std::vector<Reloc> relocs;
    std::vector<LineEnt> lines;
    std::vector<std::string> files;
    std::vector<VarEnt> vars;
    int findSym(const std::string& n) const;
};

std::vector<uint8_t> serialize(const Object& o);
Object deserialize(const std::vector<uint8_t>& bytes);
void writeObjectFile(const Object& o, const std::string& path);
Object readObjectFile(const std::string& path);

using Namer = std::function<std::string(const Operand&)>;
std::string fmtInstr(const Instr& in, const Namer& nm, bool virtualRegs = false);
std::string disasm(const Instr& in, const Object* obj = nullptr);
std::string dumpObject(const Object& o);   // objdump-style listing

}  // namespace mp
