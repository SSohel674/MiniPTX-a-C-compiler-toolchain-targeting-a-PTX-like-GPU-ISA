#pragma once
#include <cstddef>
#include "ir.h"

namespace mp {

struct OptStats {
    int folded = 0;   // constant folding / algebraic simplification
    int cse = 0;      // redundant computations replaced (local value numbering)
    int dce = 0;      // dead instructions removed (liveness based)
    int cfg = 0;      // unreachable code / redundant branches / unused labels removed
    size_t before = 0, after = 0;
};

// Runs the optimization pipeline to a fixed point on one function.
void optimize(IRFunc& f, OptStats& st);

}  // namespace mp
