#pragma once
#include "ast.h"
#include "ir.h"

namespace mp {
IRModule generateIR(const Module& m, const std::string& file);
}
