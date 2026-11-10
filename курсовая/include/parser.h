#pragma once
#include "ast.h"
#include "lexer.h"
#include <memory>

namespace db {

    class ParseError : public std::runtime_error {
    public:
        ParseError(const std::string& m) : std::runtime_error(m) {}
    };

    class Parser {
    public:
        explicit Parser(std::vector<Token> toks);
        std::unique_ptr<ast::Statement> parse_statement();

    private:
        std::vector<Token> toks_;
        size_t i_ = 0;

        const Token& cur() const { return toks_[i_]; }
        const Token& peek(size_t k = 1) const {
            return i_ + k < toks_.size() ? toks_[i_ + k] : toks_.back();
        }
        void advance() { if (i_ + 1 < toks_.size()) ++i_; }
        bool check_kw(const std::string& k) const {
            return cur().kind == Token::Kw && cur().text == k;
        }
        bool check_punct(const std::string& p) const {
            return cur().kind == Token::Punct && cur().text == p;
        }
        void expect_kw(const std::string& k);
        void expect_punct(const std::string& p);
        std::string expect_ident();

        std::unique_ptr<ast::Expr> parse_expr();
        std::unique_ptr<ast::Expr> parse_primary();
        Value parse_value_literal();

        std::unique_ptr<ast::Statement> parse_create();
        std::unique_ptr<ast::Statement> parse_drop();
        std::unique_ptr<ast::Statement> parse_use();
        std::unique_ptr<ast::Statement> parse_insert();
        std::unique_ptr<ast::Statement> parse_update();
        std::unique_ptr<ast::Statement> parse_delete();
        std::unique_ptr<ast::Statement> parse_select();
    };

}