#pragma once
#include <iostream>
#include <map>
#include <string>
#include "simulator.h"

namespace mp {

// Interactive source-level debugger (a mini cuda-gdb) driven by the .debug_line / .debug_vars sections.
class Debugger {
public:
    Debugger(Machine& m, std::istream& in, std::ostream& out) : m_(m), prog_(m.program()), in_(in), out_(out) {}
    void loop();

private:
    Machine& m_;
    const Object& prog_;
    std::istream& in_;
    std::ostream& out_;
    std::map<int, uint32_t> bps_;
    int nextBp_ = 1;
    uint32_t sel_ = 0;
    bool started_ = false;

    uint32_t lineOf(uint32_t pc) const;
    std::string funcOf(uint32_t pc) const;
    std::string sourceLine(uint32_t line) const;
    void showLoc();
    void help();
    bool setBreak(const std::string& arg);
    bool bpAt(uint32_t pc, int* id = nullptr) const;
    void resume(const std::string& mode, bool skipFirst);
    void printVar(const std::string& name);
    void printRegs();
    void printLocals();
    std::string fmtVal(uint8_t cls, uint64_t raw) const;
    const Frame* selFrame() const;
    void list(uint32_t around);
    void disas();
    void mem(const std::string& a, const std::string& n);
    void backtrace();
};

}  // namespace mp
