#include "linker.h"
#include <map>
#include "common.h"

namespace mp {

Object linkObjects(const std::vector<Object>& objs) {
    if (objs.empty()) throw CompileError("link: no input objects");
    Object out;
    out.exec = true;
    std::vector<uint32_t> base(objs.size());
    // 1. lay out .text and collect defined symbols
    std::map<std::string, int> global;  // name -> index into out.syms
    std::vector<std::vector<int>> symMap(objs.size());
    for (size_t m = 0; m < objs.size(); m++) {
        base[m] = (uint32_t)out.text.size();
        out.text.insert(out.text.end(), objs[m].text.begin(), objs[m].text.end());
        symMap[m].assign(objs[m].syms.size(), -1);
        for (size_t s = 0; s < objs[m].syms.size(); s++) {
            const Symbol& sy = objs[m].syms[s];
            if (sy.kind == SYM_UNDEF) continue;
            if (global.count(sy.name)) throw CompileError("link: multiple definition of '" + sy.name + "'");
            Symbol x = sy;
            x.value += base[m];
            out.syms.push_back(x);
            global[sy.name] = (int)out.syms.size() - 1;
            symMap[m][s] = (int)out.syms.size() - 1;
        }
    }
    // 2. resolve undefined references
    for (size_t m = 0; m < objs.size(); m++)
        for (size_t s = 0; s < objs[m].syms.size(); s++) {
            if (objs[m].syms[s].kind != SYM_UNDEF) continue;
            auto it = global.find(objs[m].syms[s].name);
            if (it == global.end()) throw CompileError("link: undefined reference to '" + objs[m].syms[s].name + "'");
            symMap[m][s] = it->second;
        }
    // 3. apply relocations
    for (size_t m = 0; m < objs.size(); m++)
        for (const Reloc& r : objs[m].relocs) {
            Operand& o = out.text[base[m] + r.instr].s[r.slot];
            int32_t target = r.sym < 0 ? r.addend + (int32_t)base[m] : (int32_t)out.syms[symMap[m][r.sym]].value;
            o = mkOp(K_LABEL, 0, 0, target);
        }
    // 4. debug info
    std::map<std::string, uint32_t> fileIdx;
    for (size_t m = 0; m < objs.size(); m++) {
        std::vector<uint32_t> fmap;
        for (auto& f : objs[m].files) {
            auto it = fileIdx.find(f);
            if (it == fileIdx.end()) { out.files.push_back(f); it = fileIdx.emplace(f, (uint32_t)out.files.size() - 1).first; }
            fmap.push_back(it->second);
        }
        for (auto& l : objs[m].lines) out.lines.push_back({l.pc + base[m], l.line, l.file < fmap.size() ? fmap[l.file] : 0});
        for (auto& v : objs[m].vars) {
            VarEnt x = v;
            x.sym = (uint32_t)symMap[m][v.sym];
            x.lo += base[m]; x.hi += base[m];
            out.vars.push_back(x);
        }
    }
    bool anyKernel = false;
    for (auto& s : out.syms) if (s.kind == SYM_KERNEL) anyKernel = true;
    if (!anyKernel) throw CompileError("link: no kernel (.entry) found in any input");
    return out;
}

}  // namespace mp
