#pragma once
#include <string>
#include <vector>
#include <stdexcept>

namespace db {

    struct Token {
        enum Kind { Ident, Number, String, Punct, Kw, End } kind;
        std::string text;
        size_t line, col;
    };

    class LexError : public std::runtime_error {
    public:
        LexError(const std::string& m) : std::runtime_error(m) {}
    };

    class Lexer {
    public:
        explicit Lexer(std::string src);
        std::vector<Token> tokenize();
    private:
        std::string src_;
        size_t pos_ = 0;
        size_t line_ = 1, col_ = 1;

        char peek() const { return pos_ < src_.size() ? src_[pos_] : '\0'; }
        char get();
        void skip_ws();
        Token make(Token::Kind k, std::string t, size_t l, size_t c);
    };

}