#pragma once
#include <string>
#include "opt.h"

namespace mp {
// source (.kc) -> PTX-style text. optLevel 0 disables the optimizer.
std::string compileToPtx(const std::string& src, const std::string& filename, int optLevel = 1,
                         OptStats* stats = nullptr, std::string* irDump = nullptr);
}
