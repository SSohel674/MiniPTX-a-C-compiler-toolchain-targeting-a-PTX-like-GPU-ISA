#pragma once
#include <string>
#include "ir.h"

namespace mp {
// Linear-scan register allocation + PTX-style text emission.
std::string emitPtx(const IRModule& m);
// Debug listing of the (virtual register) IR.
std::string dumpIR(const IRModule& m);
}
