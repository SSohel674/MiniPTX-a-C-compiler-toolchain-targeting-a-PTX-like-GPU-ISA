#include "lexer.h"
#include <cctype>
#include <cstdlib>
#include <set>
#include "common.h"

namespace mp {

static const std::set<std::string> kKeywords = {"kernel", "device", "extern", "void", "int", "float",
                                                "if", "else", "while", "for", "return"};

std::vector<Token> lex(const std::string& s) {
    std::vector<Token> out;
    size_t i = 0;
    int line = 1;
    auto push = [&](Token::Type t, const std::string& x) -> Token& {
        Token k; k.type = t; k.text = x; k.line = line; out.push_back(k); return out.back();
    };
    while (i < s.size()) {
        char c = s[i];
        if (c == '\n') { line++; i++; continue; }
        if (std::isspace((unsigned char)c)) { i++; continue; }
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') { while (i < s.size() && s[i] != '\n') i++; continue; }
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/')) { if (s[i] == '\n') line++; i++; }
            if (i + 1 >= s.size()) throw CompileError("unterminated comment", line);
            i += 2;
            continue;
        }
        if (std::isalpha((unsigned char)c) || c == '_') {
            size_t j = i;
            while (j < s.size() && (std::isalnum((unsigned char)s[j]) || s[j] == '_')) j++;
            std::string w = s.substr(i, j - i);
            push(kKeywords.count(w) ? Token::Kw : Token::Ident, w);
            i = j;
            continue;
        }
        if (std::isdigit((unsigned char)c) || (c == '.' && i + 1 < s.size() && std::isdigit((unsigned char)s[i + 1]))) {
            size_t j = i;
            bool isF = false;
            while (j < s.size() && std::isdigit((unsigned char)s[j])) j++;
            if (j < s.size() && s[j] == '.') {
                isF = true; j++;
                while (j < s.size() && std::isdigit((unsigned char)s[j])) j++;
            }
            if (j < s.size() && (s[j] == 'e' || s[j] == 'E')) {
                size_t k = j + 1;
                if (k < s.size() && (s[k] == '+' || s[k] == '-')) k++;
                if (k < s.size() && std::isdigit((unsigned char)s[k])) {
                    isF = true; j = k;
                    while (j < s.size() && std::isdigit((unsigned char)s[j])) j++;
                }
            }
            std::string num = s.substr(i, j - i);
            if (j < s.size() && (s[j] == 'f' || s[j] == 'F')) { isF = true; j++; }
            Token& t = push(isF ? Token::Float : Token::Int, num);
            if (isF) t.fval = std::strtof(num.c_str(), nullptr);
            else {
                t.ival = std::strtoll(num.c_str(), nullptr, 10);
                if (t.ival > 2147483647LL) throw CompileError("integer literal too large", line);
            }
            i = j;
            continue;
        }
        static const char* two[] = {"<=", ">=", "==", "!=", "&&", "||", "+=", "-=", "*=", "/=", "++", "--"};
        bool matched = false;
        if (i + 1 < s.size())
            for (const char* t : two)
                if (s[i] == t[0] && s[i + 1] == t[1]) { push(Token::Punct, t); i += 2; matched = true; break; }
        if (matched) continue;
        if (std::string("+-*/%<>=!(){}[];,").find(c) != std::string::npos) { push(Token::Punct, std::string(1, c)); i++; continue; }
        throw CompileError(std::string("unexpected character '") + c + "'", line);
    }
    push(Token::End, "<eof>");
    return out;
}

}  // namespace mp
