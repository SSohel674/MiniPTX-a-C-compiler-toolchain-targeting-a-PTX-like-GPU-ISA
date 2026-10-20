#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace mp {

struct Token {
    enum Type { End, Ident, Kw, Int, Float, Punct } type = End;
    std::string text;
    int line = 0;
    int64_t ival = 0;
    float fval = 0;
};

std::vector<Token> lex(const std::string& src);

}  // namespace mp
