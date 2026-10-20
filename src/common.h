#pragma once
#include <stdexcept>
#include <string>

namespace mp {

struct CompileError : std::runtime_error {
    int line;
    CompileError(const std::string& m, int l = 0) : std::runtime_error(m), line(l) {}
};

struct SimError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

}  // namespace mp
