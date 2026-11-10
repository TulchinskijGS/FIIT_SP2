#include <algorithm>   // для std::transform
#include <cctype>
#include "parser.h"
#include <sstream>

namespace db {

    Parser::Parser(std::vector<Token> toks) : toks_(std::move(toks)) {}

    void Parser::expect_kw(const std::string& k) {
        if (!check_kw(k)) throw ParseError("Expected keyword '" + k + "', got '" + cur().text + "'");
        advance();
    }

    void Parser::expect_punct(const std::string& p) {
        if (!check_punct(p)) throw ParseError("Expected '" + p + "', got '" + cur().text + "'");
        advance();
    }

    std::string Parser::expect_ident() {
        if (cur().kind != Token::Ident) throw ParseError("Expected identifier, got '" + cur().text + "'");
        std::string s = cur().text;
        advance();
        return s;
    }

    Value Parser::parse_value_literal() {
        if (cur().kind == Token::Number) {
            int64_t v = std::stoll(cur().text);
            advance();
            return v;
        }
        if (cur().kind == Token::String) {
            std::string s = cur().text;
            advance();
            return s;
        }
        if (check_kw("NULL")) { advance(); return std::monostate{}; }
        throw ParseError("Expected literal, got '" + cur().text + "'");
    }

    std::unique_ptr<ast::Expr> Parser::parse_primary() {
        if (check_punct("(")) {
            advance();
            auto e = parse_expr();
            expect_punct(")");
            return e;
        }
        if (cur().kind == Token::Number) {
            auto e = std::make_unique<ast::Literal>();
            e->v = static_cast<int64_t>(std::stoll(cur().text));
            advance();
            return e;
        }
        if (cur().kind == Token::String) {
            auto e = std::make_unique<ast::Literal>();
            e->v = cur().text;
            advance();
            return e;
        }
        if (check_kw("NULL")) {
            auto e = std::make_unique<ast::Literal>();
            e->v = std::monostate{};
            advance();
            return e;
        }
        if (cur().kind == Token::Ident) {
            auto e = std::make_unique<ast::ColumnRef>();
            e->name = cur().text;
            advance();
            return e;
        }
        throw ParseError("Unexpected token in expression: '" + cur().text + "'");
    }

    std::unique_ptr<ast::Expr> Parser::parse_expr() {
        auto lhs = parse_primary();
        if (check_kw("BETWEEN")) {
            advance();
            auto lo = parse_primary();
            expect_kw("AND");
            auto hi = parse_primary();
            auto b = std::make_unique<ast::Between>();
            b->v = std::move(lhs);
            b->lo = std::move(lo);
            b->hi = std::move(hi);
            return b;
        }
        if (check_kw("LIKE")) {
            advance();
            auto pat = parse_primary();
            auto l = std::make_unique<ast::Like>();
            l->v = std::move(lhs);
            l->pattern = std::move(pat);
            return l;
        }
        if (cur().kind == Token::Punct) {
            ast::BinOp op;
            bool is_cmp = true;
            if (cur().text == "==") op = ast::BinOp::Eq;
            else if (cur().text == "!=") op = ast::BinOp::Ne;
            else if (cur().text == "<") op = ast::BinOp::Lt;
            else if (cur().text == ">") op = ast::BinOp::Gt;
            else if (cur().text == "<=") op = ast::BinOp::Le;
            else if (cur().text == ">=") op = ast::BinOp::Ge;
            else if (cur().text == "=") op = ast::BinOp::Eq; // допускаем одиночное =
            else is_cmp = false;
            if (is_cmp) {
                advance();
                auto rhs = parse_primary();
                auto b = std::make_unique<ast::Binary>();
                b->op = op;
                b->lhs = std::move(lhs);
                b->rhs = std::move(rhs);
                return b;
            }
        }
        return lhs;
    }

    std::unique_ptr<ast::Statement> Parser::parse_create() {
        expect_kw("CREATE");
        if (check_kw("DATABASE")) {
            advance();
            auto s = std::make_unique<ast::CreateDatabase>();
            s->name = expect_ident();
            expect_punct(";");
            return s;
        }
        if (check_kw("TABLE")) {
            advance();
            auto s = std::make_unique<ast::CreateTable>();
            s->name = expect_ident();
            expect_punct("(");
            while (true) {
                ColumnDef c;
                c.name = expect_ident();
                if (check_kw("INT")) {
                    c.type = ColType::Int;
                    advance();
                } else if (check_kw("STRING")) {
                    c.type = ColType::String;
                    advance();
                } else if (cur().kind == Token::Ident) {
                    // fallback: если по какой-то причине тип пришёл как идентификатор
                    // (например, INT/STRING не добавлены в kws лексера)
                    std::string t = cur().text;
                    std::transform(t.begin(), t.end(), t.begin(), ::toupper);
                    if (t == "INT") {
                        c.type = ColType::Int;
                    } else if (t == "STRING") {
                        c.type = ColType::String;
                    } else {
                        throw ParseError("Unknown type: " + cur().text);
                    }
                    advance();
                } else {
                    throw ParseError("Expected type (INT or STRING), got '" + cur().text + "'");
                }
                while (true) {
                    if (check_kw("NOT")) {
                        advance(); expect_kw("NULL"); c.not_null = true;
                    } else if (check_kw("INDEXED")) {
                        advance(); c.indexed = true; c.not_null = true;
                    } else break;
                }
                s->columns.push_back(c);
                if (check_punct(",")) { advance(); continue; }
                break;
            }
            expect_punct(")");
            expect_punct(";");
            return s;
        }
        throw ParseError("Expected DATABASE or TABLE after CREATE");
    }

    std::unique_ptr<ast::Statement> Parser::parse_drop() {
        expect_kw("DROP");
        if (check_kw("DATABASE")) {
            advance();
            auto s = std::make_unique<ast::DropDatabase>();
            s->name = expect_ident();
            expect_punct(";");
            return s;
        }
        if (check_kw("TABLE")) {
            advance();
            auto s = std::make_unique<ast::DropTable>();
            s->name = expect_ident();
            expect_punct(";");
            return s;
        }
        throw ParseError("Expected DATABASE or TABLE after DROP");
    }

    std::unique_ptr<ast::Statement> Parser::parse_use() {
        expect_kw("USE");
        auto s = std::make_unique<ast::Use>();
        s->name = expect_ident();
        expect_punct(";");
        return s;
    }

    std::unique_ptr<ast::Statement> Parser::parse_insert() {
        expect_kw("INSERT");
        expect_kw("INTO");
        auto s = std::make_unique<ast::Insert>();
        s->table = expect_ident();
        expect_punct("(");
        while (true) {
            s->columns.push_back(expect_ident());
            if (check_punct(",")) { advance(); continue; }
            break;
        }
        expect_punct(")");
        if (!check_kw("VALUE") && !check_kw("VALUES"))
            throw ParseError("Expected VALUE(S)");
        advance();
        while (true) {
            expect_punct("(");
            std::vector<Value> row;
            while (true) {
                row.push_back(parse_value_literal());
                if (check_punct(",")) { advance(); continue; }
                break;
            }
            expect_punct(")");
            s->values.push_back(std::move(row));
            if (check_punct(",")) { advance(); continue; }
            break;
        }
        expect_punct(";");
        return s;
    }

    std::unique_ptr<ast::Statement> Parser::parse_update() {
        expect_kw("UPDATE");
        auto s = std::make_unique<ast::Update>();
        s->table = expect_ident();
        expect_kw("SET");
        while (true) {
            std::string col = expect_ident();
            if (check_punct("=")) advance();
            else throw ParseError("Expected '=' in SET");
            Value v = parse_value_literal();
            s->assignments.emplace_back(col, v);
            if (check_punct(",")) { advance(); continue; }
            break;
        }
        if (check_kw("WHERE")) {
            advance();
            s->where = parse_expr();
        }
        expect_punct(";");
        return s;
    }

    std::unique_ptr<ast::Statement> Parser::parse_delete() {
        expect_kw("DELETE");
        expect_kw("FROM");
        auto s = std::make_unique<ast::Delete>();
        s->table = expect_ident();
        if (check_kw("WHERE")) {
            advance();
            s->where = parse_expr();
        }
        expect_punct(";");
        return s;
    }

    std::unique_ptr<ast::Statement> Parser::parse_select() {
        expect_kw("SELECT");
        auto s = std::make_unique<ast::Select>();
        if (check_punct("*")) {
            advance();
            s->star = true;
        } else {
            s->star = false;
            while (true) {
                std::string col = expect_ident();
                std::string alias = col;
                if (check_kw("AS")) {
                    advance();
                    alias = expect_ident();
                }
                s->columns.emplace_back(col, alias);
                if (check_punct(",")) { advance(); continue; }
                break;
            }
        }
        expect_kw("FROM");
        s->table = expect_ident();
        if (check_kw("WHERE")) {
            advance();
            s->where = parse_expr();
        }
        expect_punct(";");
        return s;
    }

    std::unique_ptr<ast::Statement> Parser::parse_statement() {
        if (check_kw("CREATE")) return parse_create();
        if (check_kw("DROP"))   return parse_drop();
        if (check_kw("USE"))    return parse_use();
        if (check_kw("INSERT")) return parse_insert();
        if (check_kw("UPDATE")) return parse_update();
        if (check_kw("DELETE")) return parse_delete();
        if (check_kw("SELECT")) return parse_select();
        throw ParseError("Unknown statement: '" + cur().text + "'");
    }

}