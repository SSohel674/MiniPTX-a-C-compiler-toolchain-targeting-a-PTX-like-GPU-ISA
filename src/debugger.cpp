#include "debugger.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>

namespace mp {

namespace {
std::string hex32(uint32_t v) { char b[16]; std::snprintf(b, sizeof b, "0x%08x", v); return b; }
}

uint32_t Debugger::lineOf(uint32_t pc) const {
    if (pc < prog_.text.size() && prog_.text[pc].line) return prog_.text[pc].line;
    uint32_t best = 0;
    for (auto& l : prog_.lines) if (l.pc <= pc) best = l.line;
    return best;
}

std::string Debugger::funcOf(uint32_t pc) const {
    for (auto& s : prog_.syms)
        if (s.kind != SYM_UNDEF && pc >= s.value && pc < s.value + s.size) return s.name;
    return "?";
}

std::string Debugger::sourceLine(uint32_t line) const {
    static std::map<std::string, std::vector<std::string>> cache;
    if (prog_.files.empty() || line == 0) return "";
    const std::string& file = prog_.files[0];
    auto it = cache.find(file);
    if (it == cache.end()) {
        std::ifstream f(file);
        if (!f) { size_t sl = file.find_last_of("/\\"); if (sl != std::string::npos) f.open(file.substr(sl + 1)); }
        std::vector<std::string> v;
        std::string s;
        while (std::getline(f, s)) v.push_back(s);
        it = cache.emplace(file, v).first;
    }
    return line <= it->second.size() ? it->second[line - 1] : "";
}

void Debugger::help() {
    out_ << "commands:\n"
            "  break|b <line | function | *pc>   set breakpoint        info break | delete|d [n]\n"
            "  run|r | continue|c               run / resume\n"
            "  step|s | next|n | stepi|si       source step (into calls) / step over calls / one instruction\n"
            "  print|p <var | %reg>             print a variable or register of the selected thread\n"
            "  locals | regs                    all visible variables / all registers\n"
            "  thread <lane>                    select a lane of the current warp\n"
            "  where|bt | list|l | disas | mask show call stack / source / disassembly / active mask\n"
            "  mem <addr> [n]                   dump n 32-bit words of device memory\n"
            "  quit|q\n";
}

bool Debugger::bpAt(uint32_t pc, int* id) const {
    for (auto& b : bps_) if (b.second == pc) { if (id) *id = b.first; return true; }
    return false;
}

bool Debugger::setBreak(const std::string& arg) {
    uint32_t pc = UINT32_MAX;
    if (arg.empty()) { out_ << "usage: break <line|function|*pc>\n"; return false; }
    if (arg[0] == '*') pc = (uint32_t)std::strtoul(arg.c_str() + 1, nullptr, 10);
    else if (std::isdigit((unsigned char)arg[0])) {
        uint32_t line = (uint32_t)std::strtoul(arg.c_str(), nullptr, 10), bestLine = UINT32_MAX;
        for (auto& l : prog_.lines)
            if (l.line >= line && l.line < bestLine) { bestLine = l.line; pc = l.pc; }
        if (pc == UINT32_MAX) { out_ << "no code at or after line " << line << "\n"; return false; }
        for (auto& l : prog_.lines) if (l.line == bestLine && l.pc < pc) pc = l.pc;
    } else {
        int s = prog_.findSym(arg);
        if (s < 0 || prog_.syms[s].kind == SYM_UNDEF) { out_ << "unknown function '" << arg << "'\n"; return false; }
        pc = prog_.syms[s].value;
    }
    if (pc >= prog_.text.size()) { out_ << "invalid address\n"; return false; }
    bps_[nextBp_] = pc;
    out_ << "Breakpoint " << nextBp_++ << " at pc " << pc << " (" << funcOf(pc) << ", line " << lineOf(pc) << ")\n";
    return true;
}

const Frame* Debugger::selFrame() const {
    auto* me = const_cast<Debugger*>(this);
    Thread* t = me->m_.lane(sel_);
    if (!t || t->done || t->st.empty()) return nullptr;
    return &t->st.back();
}

std::string Debugger::fmtVal(uint8_t cls, uint64_t raw) const {
    char b[48];
    switch (cls) {
    case C_S32: std::snprintf(b, sizeof b, "%d", (int32_t)(uint32_t)raw); break;
    case C_F32: std::snprintf(b, sizeof b, "%g", (double)bitsToF((int32_t)(uint32_t)raw)); break;
    case C_PRED: std::snprintf(b, sizeof b, "%d", raw != 0); break;
    default: std::snprintf(b, sizeof b, "0x%llx", (unsigned long long)raw); break;
    }
    return b;
}

void Debugger::showLoc() {
    if (m_.finished()) { out_ << "Program finished (" << m_.stats().warpInstrs << " warp instructions).\n"; return; }
    auto pk = m_.peek();
    uint32_t line = lineOf(pk.pc);
    out_ << "block " << pk.block << " warp " << pk.warp << " mask " << hex32(pk.mask) << "  " << funcOf(pk.pc) << "() line " << line << "\n";
    std::string src = sourceLine(line);
    if (!src.empty()) out_ << "  " << line << " | " << src << "\n";
    out_ << "  => pc " << pk.pc << ": " << disasm(prog_.text[pk.pc], &prog_) << "\n";
}

void Debugger::resume(const std::string& mode, bool skipFirst) {
    if (m_.finished()) { out_ << "Program is not being run.\n"; return; }
    started_ = true;
    uint32_t startLine = lineOf(m_.peek().pc);
    Thread* t0 = m_.lane(sel_);
    size_t startDepth = t0 ? t0->st.size() : 0;
    bool first = skipFirst;
    for (;;) {
        if (m_.finished()) break;
        auto pk = m_.peek();
        int id;
        if (!first && bpAt(pk.pc, &id)) { out_ << "Breakpoint " << id << ", "; showLoc(); return; }
        first = false;
        try { m_.step(); }
        catch (SimError& e) { out_ << "GPU trap: " << e.what() << "\n"; return; }
        if (mode == "stepi") break;
        if (m_.finished()) break;
        if (mode == "step" || mode == "next") {
            uint32_t pc = m_.peek().pc, nl = lineOf(pc);
            Thread* t = m_.lane(sel_);
            size_t depth = t ? t->st.size() : 0;
            if (nl && nl != startLine && (mode == "step" || depth <= startDepth)) break;
        }
    }
    showLoc();
}

void Debugger::printVar(const std::string& name) {
    const Frame* f = selFrame();
    if (!f) { out_ << "selected lane has exited or does not exist\n"; return; }
    if (!name.empty() && name[0] == '%') {
        size_t i = 1;
        while (i < name.size() && std::isalpha((unsigned char)name[i])) i++;
        std::string pre = name.substr(1, i - 1);
        int cls = pre == "r" ? C_S32 : pre == "f" ? C_F32 : pre == "p" ? C_PRED : pre == "rd" ? C_U64 : -1;
        unsigned idx = (unsigned)std::atoi(name.c_str() + i);
        if (cls < 0 || idx >= f->r[cls].size()) { out_ << "no such register\n"; return; }
        out_ << name << " = " << fmtVal((uint8_t)cls, f->r[cls][idx]) << "\n";
        return;
    }
    uint32_t pc = f->pc;
    for (auto& v : prog_.vars)
        if (v.sym == f->func && v.name == name && v.lo < pc && pc <= v.hi + 0) {
            out_ << name << " = " << fmtVal(v.cls, f->r[v.cls][v.idx]) << "   (%" << regPrefix(v.cls) << v.idx << ")\n";
            return;
        }
    out_ << "no variable '" << name << "' in scope at pc " << pc << " (try 'locals')\n";
}

void Debugger::printLocals() {
    const Frame* f = selFrame();
    if (!f) { out_ << "selected lane has exited or does not exist\n"; return; }
    bool any = false;
    for (auto& v : prog_.vars)
        if (v.sym == f->func && v.lo < f->pc && f->pc <= v.hi) {
            out_ << "  " << v.name << " = " << fmtVal(v.cls, f->r[v.cls][v.idx]) << "\n";
            any = true;
        }
    if (!any) out_ << "  (no live variables)\n";
}

void Debugger::printRegs() {
    const Frame* f = selFrame();
    if (!f) { out_ << "selected lane has exited or does not exist\n"; return; }
    for (int c = 0; c < 4; c++)
        for (size_t i = 1; i < f->r[c].size(); i++)
            out_ << "  %" << regPrefix(c) << i << " = " << fmtVal((uint8_t)c, f->r[c][i]) << "\n";
}

void Debugger::list(uint32_t around) {
    if (sourceLine(1).empty() && sourceLine(around).empty()) { out_ << "source not available\n"; return; }
    uint32_t lo = around > 4 ? around - 4 : 1;
    for (uint32_t l = lo; l <= around + 4; l++) {
        std::string s = sourceLine(l);
        if (s.empty() && l > around) break;
        out_ << (l == around ? "=> " : "   ") << l << " | " << s << "\n";
    }
}

void Debugger::disas() {
    if (m_.finished()) return;
    uint32_t pc = m_.peek().pc;
    uint32_t lo = pc > 3 ? pc - 3 : 0, hi = std::min<uint32_t>((uint32_t)prog_.text.size(), pc + 5);
    for (uint32_t i = lo; i < hi; i++)
        out_ << (i == pc ? " => " : "    ") << i << ": " << disasm(prog_.text[i], &prog_) << "\n";
}

void Debugger::mem(const std::string& a, const std::string& n) {
    uint64_t addr = std::strtoull(a.c_str(), nullptr, 0);
    int cnt = n.empty() ? 4 : std::atoi(n.c_str());
    for (int i = 0; i < cnt; i++) {
        uint32_t w;
        try { m_.read(addr + 4 * i, &w, 4); } catch (SimError& e) { out_ << e.what() << "\n"; return; }
        out_ << "  0x" << std::hex << addr + 4 * i << std::dec << ": " << (int32_t)w << "  /  " << bitsToF((int32_t)w) << "\n";
    }
}

void Debugger::backtrace() {
    Thread* t = m_.lane(sel_);
    if (!t || t->done) { out_ << "selected lane has exited or does not exist\n"; return; }
    int n = 0;
    for (int i = (int)t->st.size() - 1; i >= 0; i--, n++) {
        uint32_t pc = t->st[i].pc - (i == (int)t->st.size() - 1 ? 0 : 1);
        out_ << "  #" << n << "  " << prog_.syms[t->st[i].func].name << "() at line " << lineOf(pc) << " (pc " << pc << ")\n";
    }
}

void Debugger::loop() {
    out_ << "MiniPTX debugger - " << m_.gridDim() << " block(s) x " << m_.blockDim() << " thread(s). Type 'help'.\n";
    showLoc();
    std::string line;
    for (;;) {
        out_ << "(mdb) " << std::flush;
        if (!std::getline(in_, line)) { out_ << "\n"; return; }
        std::istringstream ss(line);
        std::string cmd, a1, a2;
        ss >> cmd >> a1 >> a2;
        if (cmd.empty()) continue;
        if (cmd == "q" || cmd == "quit") return;
        else if (cmd == "help" || cmd == "h") help();
        else if (cmd == "b" || cmd == "break") setBreak(a1);
        else if (cmd == "info" && a1 == "break") {
            if (bps_.empty()) out_ << "no breakpoints\n";
            for (auto& b : bps_) out_ << "  " << b.first << ": pc " << b.second << " " << funcOf(b.second) << " line " << lineOf(b.second) << "\n";
        }
        else if (cmd == "d" || cmd == "delete") { if (a1.empty()) bps_.clear(); else bps_.erase(std::atoi(a1.c_str())); }
        else if (cmd == "r" || cmd == "run") resume("continue", false);
        else if (cmd == "c" || cmd == "continue") resume("continue", true);
        else if (cmd == "s" || cmd == "step") resume("step", true);
        else if (cmd == "n" || cmd == "next") resume("next", true);
        else if (cmd == "si" || cmd == "stepi") resume("stepi", true);
        else if (cmd == "p" || cmd == "print") printVar(a1);
        else if (cmd == "locals") printLocals();
        else if (cmd == "regs") printRegs();
        else if (cmd == "thread") {
            sel_ = (uint32_t)std::atoi(a1.c_str());
            if (!m_.lane(sel_)) out_ << "no such lane in this warp (0.." << m_.laneCount() - 1 << ")\n";
            else out_ << "selected lane " << sel_ << "\n";
        }
        else if (cmd == "where" || cmd == "bt") backtrace();
        else if (cmd == "l" || cmd == "list") { if (!m_.finished()) list(lineOf(m_.peek().pc)); }
        else if (cmd == "disas") disas();
        else if (cmd == "mask") { if (!m_.finished()) out_ << hex32(m_.peek().mask) << "\n"; }
        else if (cmd == "mem") mem(a1, a2);
        else out_ << "unknown command '" << cmd << "' (try 'help')\n";
    }
}

}  // namespace mp
