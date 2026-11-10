#pragma once
#include "ast.h"
#include "catalog.h"

namespace db {

struct QueryResult {
    bool ok = true;
    std::string message;
    std::vector<std::string> column_names;
    std::vector<Row> rows;
};

struct RangePredicate {
    bool valid = false;
    int col_idx = -1;
    bool has_lo = false;
    bool has_hi = false;
    bool lo_inclusive = true;
    bool hi_inclusive = false;
    Value lo;
    Value hi;
};

class Executor {
public:
    explicit Executor(Catalog& c) : catalog_(c) {}

    QueryResult execute(ast::Statement* stmt);

private:
    Catalog& catalog_;

    QueryResult do_create_database(ast::CreateDatabase*);
    QueryResult do_drop_database(ast::DropDatabase*);
    QueryResult do_use(ast::Use*);
    QueryResult do_create_table(ast::CreateTable*);
    QueryResult do_drop_table(ast::DropTable*);
    QueryResult do_insert(ast::Insert*);
    QueryResult do_update(ast::Update*);
    QueryResult do_delete(ast::Delete*);
    QueryResult do_select(ast::Select*);

    bool eval_expr(const ast::Expr* e, const Table& t, const Row& row, bool& out) const;

};

}