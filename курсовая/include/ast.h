#pragma once
#include "value.h"
#include "schema.h"
#include <memory>
#include <vector>
#include <string>

namespace db::ast {

    struct Expr {
        virtual ~Expr() = default;
    };

    struct Literal : Expr { Value v; };
    struct ColumnRef : Expr { std::string name; };

    enum class BinOp { Eq, Ne, Lt, Gt, Le, Ge };
    struct Binary : Expr { BinOp op; std::unique_ptr<Expr> lhs, rhs; };

    struct Between : Expr { std::unique_ptr<Expr> v, lo, hi; };
    struct Like    : Expr { std::unique_ptr<Expr> v, pattern; };

    struct Statement { virtual ~Statement() = default; };

    struct CreateDatabase : Statement { std::string name; };
    struct DropDatabase   : Statement { std::string name; };
    struct Use            : Statement { std::string name; };

    struct CreateTable : Statement { std::string name; std::vector<ColumnDef> columns; };
    struct DropTable   : Statement { std::string name; };

    struct Insert : Statement {
        std::string table;
        std::vector<std::string> columns;
        std::vector<std::vector<Value>> values;
    };

    struct Update : Statement {
        std::string table;
        std::vector<std::pair<std::string, Value>> assignments;
        std::unique_ptr<Expr> where;
    };

    struct Delete : Statement {
        std::string table;
        std::unique_ptr<Expr> where;
    };

    struct Select : Statement {
        bool star = true;
        std::vector<std::pair<std::string, std::string>> columns;
        std::string table;
        std::unique_ptr<Expr> where;
    };

}