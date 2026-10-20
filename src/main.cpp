// miniptx - command line driver for the MiniPTX toolchain.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include "assembler.h"
#include "common.h"
#include "compiler.h"
#include "debugger.h"
#include "linker.h"
#include "simulator.h"

using namespace mp;

static const char* kUsage =
    "MiniPTX - a PTX-style GPU compiler toolchain (compiler, assembler, linker, SIMT simulator, debugger)\n\n"
    "usage:\n"
    "  miniptx compile <file.kc> [-o out.ptx] [-O0|-O1] [--emit-ir] [--stats]   source -> PTX-style text\n"
    "  miniptx asm     <file.ptx> [-o out.mpo]                                  text -> relocatable object\n"
    "  miniptx link    <a.mpo> [b.mpo ...] -o prog.mpx                          objects -> executable\n"
    "  miniptx build   <a.kc> [b.kc ...] -o prog.mpx [-O0|-O1] [--keep]         compile + asm + link\n"
    "  miniptx objdump <file.mpo|file.mpx>                                      sections, symbols, relocs, disassembly\n"
    "  miniptx run     <prog.mpx> [--kernel k] --grid G --block B [--arg ...] [--print N] [--stats] [--debug]\n\n"
    "run arguments (--arg, in kernel parameter order):\n"
    "  i32:5   f32:2.5   buf:f32:COUNT:iota|ones|zeros|rand   buf:i32:COUNT:iota   buf:f32:1,2,3,4\n";

static std::string readText(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw CompileError("cannot read '" + path + "'");
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}
static void writeText(const std::string& path, const std::string& s) {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw CompileError("cannot write '" + path + "'");
    f << s;
}
static std::string stem(const std::string& p) {
    size_t sl = p.find_last_of("/\\"), dot = p.find_last_of('.');
    size_t a = sl == std::string::npos ? 0 : sl + 1;
    return p.substr(a, (dot == std::string::npos || dot < a ? p.size() : dot) - a);
}
static std::string dirOf(const std::string& p) {
    size_t sl = p.find_last_of("/\\");
    return sl == std::string::npos ? "" : p.substr(0, sl + 1);
}

struct Args {
    std::vector<std::string> pos, argvals;
    std::string out, kernel;
    int opt = 1, grid = 1, block = 32, print = 8;
    bool emitIR = false, stats = false, keep = false, debug = false;
};

static Args parseArgs(int argc, char** argv, int from) {
    Args a;
    for (int i = from; i < argc; i++) {
        std::string s = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) throw CompileError("missing value for " + s);
            return argv[++i];
        };
        if (s == "-o") a.out = val();
        else if (s == "-O0") a.opt = 0;
        else if (s == "-O1" || s == "-O2") a.opt = 1;
        else if (s == "--emit-ir") a.emitIR = true;
        else if (s == "--stats") a.stats = true;
        else if (s == "--keep") a.keep = true;
        else if (s == "--debug") a.debug = true;
        else if (s == "--kernel") a.kernel = val();
        else if (s == "--grid") a.grid = std::atoi(val().c_str());
        else if (s == "--block") a.block = std::atoi(val().c_str());
        else if (s == "--print") a.print = std::atoi(val().c_str());
        else if (s == "--arg") a.argvals.push_back(val());
        else if (!s.empty() && s[0] == '-' && s.size() > 1) throw CompileError("unknown option '" + s + "'");
        else a.pos.push_back(s);
    }
    return a;
}

static void printOptStats(const OptStats& s) {
    std::cerr << "optimizer: " << s.before << " -> " << s.after << " IR instructions  (folded " << s.folded << ", CSE " << s.cse
              << ", DCE " << s.dce << ", CFG cleanup " << s.cfg << ")\n";
}

struct Buf { uint64_t addr; bool isFloat; size_t count; };

static std::vector<std::string> splitc(const std::string& s, char d) {
    std::vector<std::string> v; std::string cur;
    for (char c : s) { if (c == d) { v.push_back(cur); cur.clear(); } else cur += c; }
    v.push_back(cur);
    return v;
}

static int cmdRun(const Args& a) {
    if (a.pos.size() != 1) throw CompileError("run: expected exactly one executable");
    Object prog = readObjectFile(a.pos[0]);
    if (!prog.exec) throw CompileError("run: '" + a.pos[0] + "' is not a linked executable (use 'miniptx link')");
    int k = -1;
    if (!a.kernel.empty()) {
        k = prog.findSym(a.kernel);
        if (k < 0 || prog.syms[k].kind != SYM_KERNEL) throw CompileError("run: no kernel named '" + a.kernel + "'");
    } else {
        for (size_t i = 0; i < prog.syms.size(); i++)
            if (prog.syms[i].kind == SYM_KERNEL) { if (k >= 0) throw CompileError("run: several kernels, pass --kernel"); k = (int)i; }
    }
    const Symbol& ks = prog.syms[k];
    if (a.argvals.size() != ks.nparams)
        throw CompileError("run: kernel '" + ks.name + "' takes " + std::to_string(ks.nparams) + " argument(s), got " + std::to_string(a.argvals.size()));
    Machine m(prog);
    std::vector<uint64_t> kargs;
    std::vector<Buf> bufs;
    unsigned seed = 12345;
    for (size_t i = 0; i < a.argvals.size(); i++) {
        auto p = splitc(a.argvals[i], ':');
        uint8_t want = ks.paramCls[i];
        if (p[0] == "i32" && p.size() == 2) {
            if (want != C_S32) throw CompileError("arg " + std::to_string(i) + ": kernel expects " + clsName(want));
            kargs.push_back((uint32_t)std::atoi(p[1].c_str()));
        } else if (p[0] == "f32" && p.size() == 2) {
            if (want != C_F32) throw CompileError("arg " + std::to_string(i) + ": kernel expects " + clsName(want));
            kargs.push_back((uint32_t)fToBits((float)std::atof(p[1].c_str())));
        } else if (p[0] == "buf" && p.size() == 4) {
            if (want != C_U64) throw CompileError("arg " + std::to_string(i) + ": kernel expects " + clsName(want));
            bool isF = p[1] == "f32";
            if (!isF && p[1] != "i32") throw CompileError("buf element type must be f32 or i32");
            std::vector<uint32_t> w;
            size_t n = (size_t)std::atol(p[2].c_str());
            for (size_t j = 0; j < n; j++) {
                double v;
                if (p[3] == "iota") v = (double)j;
                else if (p[3] == "ones") v = 1.0;
                else if (p[3] == "zeros") v = 0.0;
                else if (p[3] == "rand") v = (double)(((seed = seed * 1103515245u + 12345u) >> 16) & 0xff) / 16.0;
                else throw CompileError("unknown buffer init '" + p[3] + "'");
                w.push_back(isF ? (uint32_t)fToBits((float)v) : (uint32_t)(int32_t)v);
            }
            uint64_t addr = m.alloc(w.size() * 4);
            if (!w.empty()) m.write(addr, w.data(), w.size() * 4);
            bufs.push_back({addr, isF, w.size()});
            kargs.push_back(addr);
        } else if (p[0] == "buf" && p.size() == 3) {  // buf:f32:1,2,3
            bool isF = p[1] == "f32";
            std::vector<uint32_t> w;
            for (auto& t : splitc(p[2], ',')) w.push_back(isF ? (uint32_t)fToBits((float)std::atof(t.c_str())) : (uint32_t)std::atoi(t.c_str()));
            uint64_t addr = m.alloc(w.size() * 4);
            m.write(addr, w.data(), w.size() * 4);
            bufs.push_back({addr, isF, w.size()});
            kargs.push_back(addr);
        } else throw CompileError("bad --arg '" + a.argvals[i] + "'");
    }
    m.launch(k, (uint32_t)a.grid, (uint32_t)a.block, kargs);
    if (a.debug) {
        Debugger dbg(m, std::cin, std::cout);
        dbg.loop();
    } else {
        try { m.run(); }
        catch (SimError& e) { std::cerr << "GPU trap: " << e.what() << "\n"; return 2; }
    }
    if (!m.finished()) std::cout << "(program did not run to completion; buffers shown as-is)\n";
    for (size_t b = 0; b < bufs.size(); b++) {
        std::cout << "buf" << b << " (" << (bufs[b].isFloat ? "f32" : "i32") << " x " << bufs[b].count << "):";
        size_t n = std::min<size_t>(bufs[b].count, (size_t)std::max(0, a.print));
        for (size_t j = 0; j < n; j++) {
            uint32_t w; m.read(bufs[b].addr + 4 * j, &w, 4);
            if (bufs[b].isFloat) std::cout << " " << bitsToF((int32_t)w); else std::cout << " " << (int32_t)w;
        }
        if (n < bufs[b].count) std::cout << " ...";
        std::cout << "\n";
    }
    if (a.stats) {
        auto& s = m.stats();
        std::cerr << "simulator: " << s.warpInstrs << " warp instrs, " << s.laneInstrs << " lane instrs, " << s.divergent
                  << " divergent warp instrs, " << s.calls << " calls";
        if (s.warpInstrs) std::cerr << ", SIMD efficiency " << (100.0 * s.laneInstrs / (double)(s.warpInstrs * m.warpSize())) << "%";
        std::cerr << "\n";
    }
    return 0;
}

static Object compileAndAssemble(const std::string& path, int opt, bool keep, bool stats) {
    OptStats os;
    std::string ptx;
    try {
        ptx = compileToPtx(readText(path), path, opt, &os);
    } catch (CompileError& e) {
        throw CompileError(path + ":" + std::to_string(e.line) + ": " + e.what());
    }
    if (stats) printOptStats(os);
    if (keep) writeText(dirOf(path) + stem(path) + ".ptx", ptx);
    Object o = assemble(ptx);
    if (keep) writeObjectFile(o, dirOf(path) + stem(path) + ".mpo");
    return o;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::cout << kUsage; return 1; }
    std::string cmd = argv[1];
    try {
        if (cmd == "-h" || cmd == "--help" || cmd == "help") { std::cout << kUsage; return 0; }
        Args a = parseArgs(argc, argv, 2);
        if (cmd == "compile") {
            if (a.pos.size() != 1) throw CompileError("compile: expected one source file");
            OptStats os; std::string ir, ptx;
            try { ptx = compileToPtx(readText(a.pos[0]), a.pos[0], a.opt, &os, &ir); }
            catch (CompileError& e) { throw CompileError(a.pos[0] + ":" + std::to_string(e.line) + ": " + e.what()); }
            if (a.emitIR) std::cout << ir;
            if (a.stats) printOptStats(os);
            std::string out = a.out.empty() ? dirOf(a.pos[0]) + stem(a.pos[0]) + ".ptx" : a.out;
            writeText(out, ptx);
            std::cerr << "wrote " << out << "\n";
        } else if (cmd == "asm") {
            if (a.pos.size() != 1) throw CompileError("asm: expected one .ptx file");
            Object o;
            try { o = assemble(readText(a.pos[0])); }
            catch (CompileError& e) { throw CompileError(a.pos[0] + ":" + std::to_string(e.line) + ": " + e.what()); }
            std::string out = a.out.empty() ? dirOf(a.pos[0]) + stem(a.pos[0]) + ".mpo" : a.out;
            writeObjectFile(o, out);
            std::cerr << "wrote " << out << "\n";
        } else if (cmd == "link") {
            if (a.pos.empty() || a.out.empty()) throw CompileError("link: need input objects and -o <out.mpx>");
            std::vector<Object> objs;
            for (auto& p : a.pos) objs.push_back(readObjectFile(p));
            writeObjectFile(linkObjects(objs), a.out);
            std::cerr << "wrote " << a.out << "\n";
        } else if (cmd == "build") {
            if (a.pos.empty()) throw CompileError("build: need at least one .kc source");
            std::vector<Object> objs;
            for (auto& p : a.pos) objs.push_back(compileAndAssemble(p, a.opt, a.keep, a.stats));
            std::string out = a.out.empty() ? stem(a.pos[0]) + ".mpx" : a.out;
            writeObjectFile(linkObjects(objs), out);
            std::cerr << "wrote " << out << "\n";
        } else if (cmd == "objdump") {
            if (a.pos.size() != 1) throw CompileError("objdump: expected one file");
            std::cout << dumpObject(readObjectFile(a.pos[0]));
        } else if (cmd == "run") {
            return cmdRun(a);
        } else {
            std::cerr << "unknown command '" << cmd << "'\n\n" << kUsage;
            return 1;
        }
    } catch (CompileError& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    } catch (SimError& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 2;
    }
    return 0;
}
