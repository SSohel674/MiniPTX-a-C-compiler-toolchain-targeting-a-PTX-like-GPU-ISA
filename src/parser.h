#pragma once
#include "ast.h"
#include "lexer.h"

namespace mp {
Module parseModule(const std::vector<Token>& tokens);
}
