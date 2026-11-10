// lexer.cpp
#include "lexer.h"
#include <cctype>
#include <algorithm>

namespace db {

    Lexer::Lexer(std::string src) : src_(std::move(src)) {}

    char Lexer::get() {
        char c = peek();
        if (c == '\0') return c;
        ++pos_;
        if (c == '\n') { ++line_; col_ = 1; } else ++col_;
        return c;
    }

    void Lexer::skip_ws() {
        while (true) {
            char c = peek();
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { get(); continue; }
            if (c == '-' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '-') {
                while (peek() != '\n' && peek() != '\0') get();
                continue;
            }
            break;
        }
    }

    Token Lexer::make(Token::Kind k, std::string t, size_t l, size_t c) {
        return Token{k, std::move(t), l, c};
    }

    std::vector<Token> Lexer::tokenize() {
        std::vector<Token> out;
        static const std::vector<std::string> kws = {
            "CREATE","DROP","DATABASE","TABLE","USE","INSERT","INTO","VALUE","VALUES",
            "UPDATE","SET","DELETE","FROM","WHERE","SELECT","AS","NOT","NULL","INDEXED",
            "BETWEEN","AND","LIKE","OR","TRUE","FALSE",
            "INT","STRING"
        };
        while (true) {
            skip_ws();
            size_t l = line_, c = col_;
            char ch = peek();
            if (ch == '\0') { out.push_back(make(Token::End, "", l, c)); break; }

            if (std::isalpha(static_cast<unsigned char>(ch)) || ch == '_') {
                std::string s;
                while (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_') s += get();
                std::string up = s;
                std::transform(up.begin(), up.end(), up.begin(), ::toupper);
                bool is_kw = std::find(kws.begin(), kws.end(), up) != kws.end();
                // запрет смешения регистров в ключевом слове
                if (is_kw) {
                    bool has_lower = false, has_upper = false;
                    for (char cc : s) {
                        if (std::islower(static_cast<unsigned char>(cc))) has_lower = true;
                        if (std::isupper(static_cast<unsigned char>(cc))) has_upper = true;
                    }
                    if (has_lower && has_upper)
                        throw LexError("Mixed case in keyword: " + s);
                    out.push_back(make(Token::Kw, up, l, c));
                } else {
                    out.push_back(make(Token::Ident, s, l, c));
                }
                continue;
            }
            if (std::isdigit(static_cast<unsigned char>(ch))) {
                std::string s;
                while (std::isdigit(static_cast<unsigned char>(peek()))) s += get();
                out.push_back(make(Token::Number, s, l, c));
                continue;
            }
            if (ch == '"') {
                get();
                std::string s;
                while (peek() != '"' && peek() != '\0') {
                    if (peek() == '\\') { get(); s += get(); }
                    else s += get();
                }
                if (peek() != '"') throw LexError("Unterminated string literal");
                get();
                out.push_back(make(Token::String, s, l, c));
                continue;
            }
            std::string two;
            if (pos_ + 1 < src_.size()) two = std::string(1, ch) + src_[pos_ + 1];
            if (two == "==" || two == "!=" || two == "<=" || two == ">=") {
                get(); get();
                out.push_back(make(Token::Punct, two, l, c));
                continue;
            }
            if (std::string("()*,;=<>.").find(ch) != std::string::npos) {
                get();
                out.push_back(make(Token::Punct, std::string(1, ch), l, c));
                continue;
            }
            throw LexError(std::string("Unexpected character: ") + ch);
        }
        return out;
    }

}