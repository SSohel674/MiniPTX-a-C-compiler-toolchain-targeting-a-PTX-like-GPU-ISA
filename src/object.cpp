#include "object.h"
#include <cstdio>
#include <fstream>
#include <sstream>
#include "common.h"

namespace mp {

int Object::findSym(const std::string& n) const {
    for (size_t i = 0; i < syms.size(); i++)
        if (syms[i].name == n) return (int)i;
    return -1;
}

// ---------------------------------------------------------------- binary format
#pragma pack(push, 1)
struct FileHdr { char magic[4]; uint16_t version, type; uint32_t nsec; };
struct SecHdr { char name[16]; uint32_t off, size; };
struct SymRec { uint32_t name, value, size; uint8_t kind, nparams, retCls, pad; uint8_t paramCls[8]; uint16_t nregs[4]; };
struct RelRec { uint32_t instr; uint8_t slot; int32_t sym, addend; };
struct LineRec { uint32_t pc, line, file; };
struct VarRec { uint32_t sym; uint8_t cls; uint16_t idx; uint32_t name, lo, hi; };
#pragma pack(pop)

namespace {
struct StrTab {
    std::string data;
    uint32_t add(const std::string& s) { uint32_t o = (uint32_t)data.size(); data += s; data.push_back('\0'); return o; }
};
template <class T> void put(std::vector<uint8_t>& v, const T& x) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&x);
    v.insert(v.end(), p, p + sizeof(T));
}
}  // namespace

std::vector<uint8_t> serialize(const Object& o) {
    StrTab st;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> secs;

    std::vector<uint8_t> text;
    for (auto& i : o.text) put(text, i);
    secs.push_back({".text", text});

    std::vector<uint8_t> sy;
    for (auto& s : o.syms) {
        SymRec r{};
        r.name = st.add(s.name); r.value = s.value; r.size = s.size;
        r.kind = s.kind; r.nparams = s.nparams; r.retCls = s.retCls;
        std::memcpy(r.paramCls, s.paramCls, 8);
        for (int k = 0; k < 4; k++) r.nregs[k] = s.nregs[k];
        put(sy, r);
    }
    secs.push_back({".symtab", sy});

    std::vector<uint8_t> rel;
    for (auto& x : o.relocs) { RelRec r{x.instr, x.slot, x.sym, x.addend}; put(rel, r); }
    secs.push_back({".rela.text", rel});

    std::vector<uint8_t> ln;
    for (auto& x : o.lines) { LineRec r{x.pc, x.line, x.file}; put(ln, r); }
    secs.push_back({".debug_line", ln});

    std::vector<uint8_t> fl;
    for (auto& f : o.files) put(fl, st.add(f));
    secs.push_back({".debug_file", fl});

    std::vector<uint8_t> vr;
    for (auto& v : o.vars) { VarRec r{v.sym, v.cls, v.idx, st.add(v.name), v.lo, v.hi}; put(vr, r); }
    secs.push_back({".debug_vars", vr});

    secs.push_back({".strtab", std::vector<uint8_t>(st.data.begin(), st.data.end())});

    FileHdr h{};
    std::memcpy(h.magic, o.exec ? "MPTX" : "MPTO", 4);
    h.version = 1; h.type = o.exec ? 2 : 1; h.nsec = (uint32_t)secs.size();
    std::vector<uint8_t> out;
    put(out, h);
    uint32_t off = (uint32_t)(sizeof(FileHdr) + secs.size() * sizeof(SecHdr));
    for (auto& s : secs) {
        SecHdr sh{};
        std::snprintf(sh.name, sizeof(sh.name), "%s", s.first.c_str());
        sh.off = off; sh.size = (uint32_t)s.second.size();
        put(out, sh);
        off += sh.size;
    }
    for (auto& s : secs) out.insert(out.end(), s.second.begin(), s.second.end());
    return out;
}

Object deserialize(const std::vector<uint8_t>& b) {
    if (b.size() < sizeof(FileHdr)) throw CompileError("not a MiniPTX object: file too small");
    FileHdr h;
    std::memcpy(&h, b.data(), sizeof(h));
    bool exec = std::memcmp(h.magic, "MPTX", 4) == 0;
    if (!exec && std::memcmp(h.magic, "MPTO", 4) != 0) throw CompileError("not a MiniPTX object: bad magic");
    if (h.version != 1) throw CompileError("unsupported object version");
    std::vector<SecHdr> sh(h.nsec);
    if (sizeof(FileHdr) + h.nsec * sizeof(SecHdr) > b.size()) throw CompileError("corrupt object: section table");
    std::memcpy(sh.data(), b.data() + sizeof(FileHdr), h.nsec * sizeof(SecHdr));
    auto sec = [&](const char* name) -> std::pair<const uint8_t*, size_t> {
        for (auto& s : sh)
            if (std::strncmp(s.name, name, sizeof(s.name)) == 0) {
                if ((size_t)s.off + s.size > b.size()) throw CompileError("corrupt object: section bounds");
                return {b.data() + s.off, s.size};
            }
        throw CompileError(std::string("corrupt object: missing section ") + name);
    };
    auto str = [&](uint32_t off) {
        auto s = sec(".strtab");
        if (off >= s.second) throw CompileError("corrupt object: string offset");
        return std::string(reinterpret_cast<const char*>(s.first + off));
    };
    Object o;
    o.exec = exec;
    { auto s = sec(".text"); o.text.resize(s.second / sizeof(Instr)); if (!o.text.empty()) std::memcpy(o.text.data(), s.first, o.text.size() * sizeof(Instr)); }
    { auto s = sec(".symtab");
      for (size_t i = 0; i + sizeof(SymRec) <= s.second; i += sizeof(SymRec)) {
          SymRec r; std::memcpy(&r, s.first + i, sizeof(r));
          Symbol x; x.name = str(r.name); x.value = r.value; x.size = r.size; x.kind = r.kind;
          x.nparams = r.nparams; x.retCls = r.retCls; std::memcpy(x.paramCls, r.paramCls, 8);
          for (int k = 0; k < 4; k++) x.nregs[k] = r.nregs[k];
          o.syms.push_back(x);
      } }
    { auto s = sec(".rela.text");
      for (size_t i = 0; i + sizeof(RelRec) <= s.second; i += sizeof(RelRec)) {
          RelRec r; std::memcpy(&r, s.first + i, sizeof(r));
          Reloc x; x.instr = r.instr; x.slot = r.slot; x.sym = r.sym; x.addend = r.addend;
          o.relocs.push_back(x);
      } }
    { auto s = sec(".debug_line");
      for (size_t i = 0; i + sizeof(LineRec) <= s.second; i += sizeof(LineRec)) {
          LineRec r; std::memcpy(&r, s.first + i, sizeof(r));
          o.lines.push_back({r.pc, r.line, r.file});
      } }
    { auto s = sec(".debug_file");
      for (size_t i = 0; i + 4 <= s.second; i += 4) { uint32_t off; std::memcpy(&off, s.first + i, 4); o.files.push_back(str(off)); } }
    { auto s = sec(".debug_vars");
      for (size_t i = 0; i + sizeof(VarRec) <= s.second; i += sizeof(VarRec)) {
          VarRec r; std::memcpy(&r, s.first + i, sizeof(r));
          VarEnt x; x.sym = r.sym; x.cls = r.cls; x.idx = r.idx; x.name = str(r.name); x.lo = r.lo; x.hi = r.hi;
          o.vars.push_back(x);
      } }
    return o;
}

void writeObjectFile(const Object& o, const std::string& path) {
    auto b = serialize(o);
    std::ofstream f(path, std::ios::binary);
    if (!f) throw CompileError("cannot write '" + path + "'");
    f.write(reinterpret_cast<const char*>(b.data()), (std::streamsize)b.size());
}

Object readObjectFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw CompileError("cannot read '" + path + "'");
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return deserialize(b);
}

// ---------------------------------------------------------------- text formatting
static const char* kSpecial[] = {"%tid.x", "%ctaid.x", "%ntid.x", "%nctaid.x"};
static const char* kCmp[] = {"lt", "le", "gt", "ge", "eq", "ne"};
static const char* kOpName[OP_COUNT] = {"nop", "mov", "add", "sub", "mul", "div", "rem", "neg", "and", "or", "not",
                                        "setp", "cvt", "mul.wide", "ld.param", "ld.global", "st.global", "bra",
                                        "call", "ret"};

static std::string fmtOperand(const Operand& o, const Namer& nm, bool virt) {
    switch (o.kind) {
    case K_REG: return std::string("%") + (virt ? "v" : regPrefix(o.cls)) + std::to_string(o.idx);
    case K_IMM: {
        if (o.cls == C_F32) { char b[32]; std::snprintf(b, sizeof b, "0f%08X", (uint32_t)o.imm); return b; }
        return std::to_string(o.imm);
    }
    case K_SPECIAL: return kSpecial[o.imm & 3];
    default: return nm(o);
    }
}

std::string fmtInstr(const Instr& in, const Namer& nm, bool virt) {
    auto O = [&](int k) { return fmtOperand(in.s[k], nm, virt); };
    auto D = [&]() { return fmtOperand(in.dst, nm, virt); };
    std::string T = clsName(in.type), b;
    switch (in.op) {
    case OP_MOV: b = "mov." + T + " " + D() + ", " + O(0); break;
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_REM: case OP_AND: case OP_OR:
        b = std::string(kOpName[in.op]) + "." + T + " " + D() + ", " + O(0) + ", " + O(1); break;
    case OP_NEG: case OP_NOT: b = std::string(kOpName[in.op]) + "." + T + " " + D() + ", " + O(0); break;
    case OP_SETP: b = std::string("setp.") + kCmp[in.cmp % 6] + "." + T + " " + D() + ", " + O(0) + ", " + O(1); break;
    case OP_CVT: b = "cvt." + T + "." + clsName(in.cmp) + " " + D() + ", " + O(0); break;
    case OP_MUL_WIDE: b = "mul.wide.s32 " + D() + ", " + O(0) + ", " + O(1); break;
    case OP_LD_PARAM: b = "ld.param." + T + " " + D() + ", [" + O(0) + "]"; break;
    case OP_LD_GLOBAL: b = "ld.global." + T + " " + D() + ", [" + O(0) + "]"; break;
    case OP_ST_GLOBAL: b = "st.global." + T + " [" + O(0) + "], " + O(1); break;
    case OP_BRA: b = "bra " + O(0); break;
    case OP_CALL: {
        b = "call." + T + " ";
        if (in.type != C_VOID) b += D() + ", ";
        b += O(0);
        for (int k = 1; k < 5 && in.s[k].kind != K_NONE; k++) b += ", " + O(k);
        break;
    }
    case OP_RET: b = in.type == C_VOID ? "ret" : "ret." + T + " " + O(0); break;
    default: b = "nop"; break;
    }
    if (in.predmode) b = std::string("@") + (in.predmode == 2 ? "!" : "") + fmtOperand(in.pred, nm, virt) + " " + b;
    // human-readable note for float immediates
    for (int k = 0; k < 5; k++)
        if (in.s[k].kind == K_IMM && in.s[k].cls == C_F32) {
            char n[48]; std::snprintf(n, sizeof n, "  // %g", (double)bitsToF(in.s[k].imm));
            b += n; break;
        }
    return b;
}

std::string disasm(const Instr& in, const Object* obj) {
    Namer nm = [obj, &in](const Operand& o) -> std::string {
        if (o.kind == K_LABEL) {
            if (obj && obj->exec && in.op == OP_CALL)
                for (auto& sy : obj->syms) if (sy.kind != SYM_UNDEF && (int32_t)sy.value == o.imm) return sy.name;
            return "@" + std::to_string(o.imm);
        }
        if (o.kind == K_SYM) {
            if (obj && o.imm >= 0 && (size_t)o.imm < obj->syms.size()) return obj->syms[o.imm].name;
            return "sym#" + std::to_string(o.imm);
        }
        if (o.kind == K_PARAM) return "param" + std::to_string(o.idx);
        return "?";
    };
    return fmtInstr(in, nm, false);
}

std::string dumpObject(const Object& o) {
    std::ostringstream os;
    os << "file format: " << (o.exec ? "MPTX (executable)" : "MPTO (relocatable)") << "\n";
    os << "sections: .text(" << o.text.size() << " instrs x 64B) .symtab(" << o.syms.size() << ") .rela.text("
       << o.relocs.size() << ") .debug_line(" << o.lines.size() << ") .debug_vars(" << o.vars.size() << ")\n\n";
    os << "SYMBOL TABLE:\n";
    for (size_t i = 0; i < o.syms.size(); i++) {
        auto& s = o.syms[i];
        const char* k = s.kind == SYM_KERNEL ? "kernel" : s.kind == SYM_DEVICE ? "device" : "UNDEF ";
        os << "  [" << i << "] " << k << "  " << s.name << "  value=" << s.value << " size=" << s.size
           << " params=" << (int)s.nparams << " ret=" << clsName(s.retCls) << " regs(r/f/p/rd)=" << s.nregs[0] << "/"
           << s.nregs[1] << "/" << s.nregs[2] << "/" << s.nregs[3] << "\n";
    }
    if (!o.relocs.empty()) {
        os << "\nRELOCATIONS (.rela.text):\n";
        for (auto& r : o.relocs) {
            os << "  instr " << r.instr << " slot " << (int)r.slot << "  ";
            if (r.sym < 0) os << "R_TEXT_REL  addend=" << r.addend << "\n";
            else os << "R_SYM_ABS   " << o.syms[r.sym].name << "\n";
        }
    }
    os << "\nDISASSEMBLY (.text):\n";
    size_t li = 0;
    for (size_t pc = 0; pc < o.text.size(); pc++) {
        for (auto& s : o.syms)
            if (s.kind != SYM_UNDEF && s.value == pc) os << "<" << s.name << ">:\n";
        while (li + 1 < o.lines.size() && o.lines[li + 1].pc <= pc) li++;
        char buf[32]; std::snprintf(buf, sizeof buf, "  %4zu: ", pc);
        os << buf << disasm(o.text[pc], &o);
        os << "    ; line " << o.text[pc].line << "\n";
    }
    if (!o.vars.empty()) {
        os << "\nDEBUG VARS:\n";
        for (auto& v : o.vars)
            os << "  " << o.syms[v.sym].name << "::" << v.name << " -> %" << regPrefix(v.cls) << v.idx << " live pc [" << v.lo << ", " << v.hi << "]\n";
    }
    return os.str();
}

}  // namespace mp
