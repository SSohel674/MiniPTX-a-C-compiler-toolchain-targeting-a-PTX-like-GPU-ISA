#pragma once
#include <string>
#include "object.h"

namespace mp {
// PTX-style text -> relocatable object (.mpo): binary instructions, symbols, relocations, debug info.
Object assemble(const std::string& text);
}
