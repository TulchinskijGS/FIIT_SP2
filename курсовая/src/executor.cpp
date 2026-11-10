#include "executor.h"
#include <regex>
#include <sstream>
#include <algorithm>
#include <stdexcept>

namespace db {

    namespace {

        bool compare_values(const Value& a, const Value& b, ast::BinOp op) {
            if (is_null(a) || is_null(b)) return false;
            int cmp = 0;
            if (a.index() != b.index()) {
                cmp = (a.index() < b.index()) ? -1 : 1;
            } else if (std::holds_alternative<int64_t>(a)) {
                auto x = std::get<int64_t>(a);
                auto y = std::get<int64_t>(b);
                cmp = (x < y) ? -1 : (x > y ? 1 : 0);
            } else {
                const auto& x = std::get<std::string>(a);
                const auto& y = std::get<std::string>(b);
                int r = x.compare(y);
                cmp = (r < 0) ? -1 : (r > 0 ? 1 : 0);
            }
            switch (op) {
                case ast::BinOp::Eq: return cmp == 0;
                case ast::BinOp::Ne: return cmp != 0;
                case ast::BinOp::Lt: return cmp < 0;
                case ast::BinOp::Gt: return cmp > 0;
                case ast::BinOp::Le: return cmp <= 0;
                case ast::BinOp::Ge: return cmp >= 0;
            }
            return false;
        }

        RangePredicate extract_range(const ast::Expr* e, const TableSchema& schema) {
            RangePredicate r;
            if (auto bin = dynamic_cast<const ast::Binary*>(e)) {
                const ast::ColumnRef* col = nullptr;
                const ast::Literal*   lit = nullptr;
                bool col_on_left = true;

                if ((col = dynamic_cast<const ast::ColumnRef*>(bin->lhs.get()))) {
                    lit = dynamic_cast<const ast::Literal*>(bin->rhs.get());
                } else if ((col = dynamic_cast<const ast::ColumnRef*>(bin->rhs.get()))) {
                    lit = dynamic_cast<const ast::Literal*>(bin->lhs.get());
                    col_on_left = false;
                }
                if (!col || !lit) return r;

                int ci = schema.find_col(col->name);
                if (ci < 0) return r;

                ast::BinOp op = bin->op;
                if (!col_on_left) {
                    switch (op) {
                        case ast::BinOp::Lt: op = ast::BinOp::Gt; break;
                        case ast::BinOp::Gt: op = ast::BinOp::Lt; break;
                        case ast::BinOp::Le: op = ast::BinOp::Ge; break;
                        case ast::BinOp::Ge: op = ast::BinOp::Le; break;
                        default: break; // Eq/Ne симметричны
                    }
                }

                r.col_idx = ci;
                switch (op) {
                    case ast::BinOp::Eq:
                        r.has_lo = r.has_hi = true;
                        r.lo_inclusive = true;
                        r.hi_inclusive = true;
                        r.lo = r.hi = lit->v;
                        r.valid = true;
                        return r;
                    case ast::BinOp::Lt:
                        r.has_hi = true;
                        r.hi_inclusive = false;
                        r.hi = lit->v;
                        r.valid = true;
                        return r;
                    case ast::BinOp::Le:
                        r.has_hi = true;
                        r.hi_inclusive = true;
                        r.hi = lit->v;
                        r.valid = true;
                        return r;
                    case ast::BinOp::Gt:
                        r.has_lo = true;
                        r.lo_inclusive = false;
                        r.lo = lit->v;
                        r.valid = true;
                        return r;
                    case ast::BinOp::Ge:
                        r.has_lo = true;
                        r.lo_inclusive = true;
                        r.lo = lit->v;
                        r.valid = true;
                        return r;
                    default:
                        return r;
                }
            }

            if (auto bt = dynamic_cast<const ast::Between*>(e)) {
                auto col = dynamic_cast<const ast::ColumnRef*>(bt->v.get());
                auto lo  = dynamic_cast<const ast::Literal*>(bt->lo.get());
                auto hi  = dynamic_cast<const ast::Literal*>(bt->hi.get());
                if (!col || !lo || !hi) return r;

                int ci = schema.find_col(col->name);
                if (ci < 0) return r;

                r.col_idx     = ci;
                r.has_lo      = true;
                r.has_hi      = true;
                r.lo_inclusive = true;
                r.hi_inclusive = false;
                r.lo = lo->v;
                r.hi = hi->v;
                r.valid = true;
                return r;
            }

            return r;
        }

        bool resolve_expr(const ast::Expr* e, const Table& t, const Row& row, Value& out) {
            if (auto lit = dynamic_cast<const ast::Literal*>(e)) {
                out = lit->v;
                return true;
            }
            if (auto col = dynamic_cast<const ast::ColumnRef*>(e)) {
                int idx = t.schema().find_col(col->name);
                if (idx < 0) return false;
                out = row[static_cast<size_t>(idx)];
                return true;
            }
            return false;
        }

    }

    bool Executor::eval_expr(const ast::Expr* e, const Table& t, const Row& row, bool& out) const {
        if (auto lit = dynamic_cast<const ast::Literal*>(e)) {
            if (is_null(lit->v)) { out = false; return true; }
            if (auto p = std::get_if<int64_t>(&lit->v)) { out = (*p != 0); return true; }
            out = !std::get<std::string>(lit->v).empty();
            return true;
        }

        if (auto col = dynamic_cast<const ast::ColumnRef*>(e)) {
            int idx = t.schema().find_col(col->name);
            if (idx < 0) return false;
            const Value& v = row[static_cast<size_t>(idx)];
            if (is_null(v)) { out = false; return true; }
            if (auto p = std::get_if<int64_t>(&v)) { out = (*p != 0); return true; }
            out = !std::get<std::string>(v).empty();
            return true;
        }

        if (auto bin = dynamic_cast<const ast::Binary*>(e)) {
            Value lv, rv;
            if (!resolve_expr(bin->lhs.get(), t, row, lv)) return false;
            if (!resolve_expr(bin->rhs.get(), t, row, rv)) return false;
            out = compare_values(lv, rv, bin->op);
            return true;
        }

        if (auto bt = dynamic_cast<const ast::Between*>(e)) {
            Value v, lo, hi;
            if (!resolve_expr(bt->v.get(),  t, row, v))  return false;
            if (!resolve_expr(bt->lo.get(), t, row, lo)) return false;
            if (!resolve_expr(bt->hi.get(), t, row, hi)) return false;
            out = compare_values(v, lo, ast::BinOp::Ge) &&
                compare_values(v, hi, ast::BinOp::Lt);
            return true;
        }

        if (auto lk = dynamic_cast<const ast::Like*>(e)) {
            Value v, pat;
            if (!resolve_expr(lk->v.get(),       t, row, v))   return false;
            if (!resolve_expr(lk->pattern.get(), t, row, pat)) return false;
            if (!std::holds_alternative<std::string>(v) ||
                !std::holds_alternative<std::string>(pat))
                return false;
            try {
                std::regex re(std::get<std::string>(pat));
                out = std::regex_search(std::get<std::string>(v), re);
            } catch (...) {
                out = false;
            }
            return true;
        }

        return false;
    }

    QueryResult Executor::execute(ast::Statement* stmt) {
        try {
            if (auto s = dynamic_cast<ast::CreateDatabase*>(stmt)) return do_create_database(s);
            if (auto s = dynamic_cast<ast::DropDatabase*>(stmt))   return do_drop_database(s);
            if (auto s = dynamic_cast<ast::Use*>(stmt))            return do_use(s);
            if (auto s = dynamic_cast<ast::CreateTable*>(stmt))    return do_create_table(s);
            if (auto s = dynamic_cast<ast::DropTable*>(stmt))      return do_drop_table(s);
            if (auto s = dynamic_cast<ast::Insert*>(stmt))         return do_insert(s);
            if (auto s = dynamic_cast<ast::Update*>(stmt))         return do_update(s);
            if (auto s = dynamic_cast<ast::Delete*>(stmt))         return do_delete(s);
            if (auto s = dynamic_cast<ast::Select*>(stmt))         return do_select(s);
        } catch (const std::exception& e) {
            QueryResult r;
            r.ok = false;
            r.message = e.what();
            return r;
        }
        QueryResult r;
        r.ok = false;
        r.message = "Unknown statement type";
        return r;
    }

    QueryResult Executor::do_create_database(ast::CreateDatabase* s) {
        QueryResult r;
        if (catalog_.create_database(s->name)) {
            r.ok = true;
            r.message = "Database created: " + s->name;
        } else {
            r.ok = false;
            r.message = "Database already exists: " + s->name;
        }
        return r;
    }

    QueryResult Executor::do_drop_database(ast::DropDatabase* s) {
        QueryResult r;
        if (catalog_.drop_database(s->name)) {
            r.ok = true;
            r.message = "Database dropped: " + s->name;
        } else {
            r.ok = false;
            r.message = "Database not found: " + s->name;
        }
        return r;
    }

    QueryResult Executor::do_use(ast::Use* s) {
        QueryResult r;
        if (catalog_.use_database(s->name)) {
            r.ok = true;
            r.message = "Using database: " + s->name;
        } else {
            r.ok = false;
            r.message = "Database not found: " + s->name;
        }
        return r;
    }

    QueryResult Executor::do_create_table(ast::CreateTable* s) {
        QueryResult r;

        TableSchema schema;
        schema.name = s->name;

        for (const auto& c : s->columns) {
            if (schema.name_to_idx.count(c.name)) {
                r.ok = false;
                r.message = "Duplicate column: " + c.name;
                return r;
            }
            ColumnDef col = c;
            if (col.indexed) col.not_null = true;

            schema.name_to_idx[col.name] = schema.columns.size();
            schema.columns.push_back(col);
        }

        if (catalog_.create_table(schema)) {
            r.ok = true;
            r.message = "Table created: " + s->name;
        } else {
            r.ok = false;
            r.message = "Table already exists or no database selected: " + s->name;
        }
        return r;
    }

    QueryResult Executor::do_drop_table(ast::DropTable* s) {
        QueryResult r;
        if (catalog_.drop_table(s->name)) {
            r.ok = true;
            r.message = "Table dropped: " + s->name;
        } else {
            r.ok = false;
            r.message = "Table not found: " + s->name;
        }
        return r;
    }

    QueryResult Executor::do_insert(ast::Insert* s) {
        QueryResult r;

        Table* t = catalog_.get_table(s->table);
        if (!t) {
            r.ok = false;
            r.message = "Table not found: " + s->table;
            return r;
        }

        const TableSchema& schema = t->schema();
        std::vector<int> col_idx;
        col_idx.reserve(s->columns.size());
        for (const auto& name : s->columns) {
            int idx = schema.find_col(name);
            if (idx < 0) {
                r.ok = false;
                r.message = "Unknown column: " + name;
                return r;
            }
            col_idx.push_back(idx);
        }

        size_t inserted = 0;
        for (const auto& in_row : s->values) {
            if (in_row.size() != col_idx.size()) {
                r.ok = false;
                r.message = "Column/value count mismatch";
                return r;
            }
            Row row(schema.columns.size(), std::monostate{});

            for (size_t k = 0; k < col_idx.size(); ++k) {
                int ci = col_idx[k];
                const ColumnDef& def = schema.columns[static_cast<size_t>(ci)];
                const Value& v = in_row[k];

                if (is_null(v)) {
                    if (def.not_null) {
                        r.ok = false;
                        r.message = "NULL not allowed for column: " + def.name;
                        return r;
                    }
                    row[static_cast<size_t>(ci)] = std::monostate{};
                    continue;
                }

                if (def.type == ColType::Int) {
                    if (!std::holds_alternative<int64_t>(v)) {
                        r.ok = false;
                        r.message = "Type mismatch (expected INT) for column: " + def.name;
                        return r;
                    }
                } else {
                    if (!std::holds_alternative<std::string>(v)) {
                        r.ok = false;
                        r.message = "Type mismatch (expected STRING) for column: " + def.name;
                        return r;
                    }
                }
                row[static_cast<size_t>(ci)] = v;
            }
            for (size_t ci = 0; ci < row.size(); ++ci) {
                if (is_null(row[ci]) && schema.columns[ci].not_null) {
                    r.ok = false;
                    r.message = "Missing NOT NULL column: " + schema.columns[ci].name;
                    return r;
                }
            }

            for (size_t ci = 0; ci < schema.columns.size(); ++ci) {
                if (!schema.columns[ci].indexed) continue;
                const Index* idx = t->find_index(ci);
                if (!idx) continue;
                const Value& key = row[ci];
                if (is_null(key)) {
                    r.ok = false;
                    r.message = "INDEXED column cannot be NULL: " + schema.columns[ci].name;
                    return r;
                }
                if (idx->tree.contains(key)) {
                    r.ok = false;
                    r.message = "Duplicate value for indexed column: " + schema.columns[ci].name;
                    return r;
                }
            }

            t->insert_row(row);
            ++inserted;
        }

        catalog_.persist(*t);

        r.ok = true;
        r.message = "Inserted " + std::to_string(inserted) + " row(s)";
        return r;
    }

    QueryResult Executor::do_update(ast::Update* s) {
        QueryResult r;

        Table* t = catalog_.get_table(s->table);
        if (!t) {
            r.ok = false;
            r.message = "Table not found: " + s->table;
            return r;
        }

        const TableSchema& schema = t->schema();

        struct Assign { size_t ci; Value v; };
        std::vector<Assign> assigns;
        for (const auto& [name, v] : s->assignments) {
            int idx = schema.find_col(name);
            if (idx < 0) {
                r.ok = false;
                r.message = "Unknown column: " + name;
                return r;
            }
            const ColumnDef& def = schema.columns[static_cast<size_t>(idx)];
            if (is_null(v) && def.not_null) {
                r.ok = false;
                r.message = "NULL not allowed for column: " + name;
                return r;
            }
            if (!is_null(v)) {
                if (def.type == ColType::Int && !std::holds_alternative<int64_t>(v)) {
                    r.ok = false;
                    r.message = "Type mismatch for column: " + name;
                    return r;
                }
                if (def.type == ColType::String && !std::holds_alternative<std::string>(v)) {
                    r.ok = false;
                    r.message = "Type mismatch for column: " + name;
                    return r;
                }
            }
            assigns.push_back({static_cast<size_t>(idx), v});
        }

        auto all = t->scan_all();

        size_t updated = 0;
        for (auto& [rid, row] : all) {
            bool match = true;
            if (s->where) {
                if (!eval_expr(s->where.get(), *t, row, match)) {
                    r.ok = false;
                    r.message = "Failed to evaluate WHERE";
                    return r;
                }
            }
            if (!match) continue;

            for (const auto& a : assigns) {
                const ColumnDef& def = schema.columns[a.ci];
                if (!def.indexed) continue;
                const Index* idx = t->find_index(a.ci);
                if (!idx) continue;
                if (is_null(a.v)) {
                    r.ok = false;
                    r.message = "INDEXED cannot be NULL: " + def.name;
                    return r;
                }
                if (!compare_values(a.v, row[a.ci], ast::BinOp::Eq) &&
                    idx->tree.contains(a.v)) {
                    r.ok = false;
                    r.message = "Duplicate value for indexed column: " + def.name;
                    return r;
                }
            }
            Row new_row = row;
            for (const auto& a : assigns)
                new_row[a.ci] = a.v;
            if (!t->update_row(rid, new_row)) {
                r.ok = false;
                r.message = "Failed to update row";
                return r;
            }
            ++updated;
        }

        catalog_.persist(*t);

        r.ok = true;
        r.message = "Updated " + std::to_string(updated) + " row(s)";
        return r;
    }

    QueryResult Executor::do_delete(ast::Delete* s) {
        QueryResult r;

        Table* t = catalog_.get_table(s->table);
        if (!t) {
            r.ok = false;
            r.message = "Table not found: " + s->table;
            return r;
        }

        auto all = t->scan_all();

        size_t deleted = 0;
        for (auto& [rid, row] : all) {
            bool match = true;
            if (s->where) {
                if (!eval_expr(s->where.get(), *t, row, match)) {
                    r.ok = false;
                    r.message = "Failed to evaluate WHERE";
                    return r;
                }
            }
            if (!match) continue;

            if (!t->delete_row(rid)) {
                r.ok = false;
                r.message = "Failed to delete row";
                return r;
            }
            ++deleted;
        }

        catalog_.persist(*t);

        r.ok = true;
        r.message = "Deleted " + std::to_string(deleted) + " row(s)";
        return r;
    }

    QueryResult Executor::do_select(ast::Select* s) {
        QueryResult r;

        Table* t = catalog_.get_table(s->table);
        if (!t) {
            r.ok = false;
            r.message = "Table not found: " + s->table;
            return r;
        }

        const TableSchema& schema = t->schema();

        if (s->star) {
            for (const auto& c : schema.columns)
                r.column_names.push_back(c.name);
        } else {
            for (const auto& [col, alias] : s->columns) {
                if (schema.find_col(col) < 0) {
                    r.ok = false;
                    r.message = "Unknown column: " + col;
                    return r;
                }
                r.column_names.push_back(alias);
            }
        }

        std::vector<std::pair<RowId, Row>> candidates;
        bool used_index = false;

        if (s->where) {
            RangePredicate rp = extract_range(s->where.get(), schema);

            if (rp.valid && rp.col_idx >= 0) {
                if (Index* idx = t->find_index(static_cast<size_t>(rp.col_idx))) {
                    auto& tree = idx->tree;

                    auto it_begin = rp.has_lo
                        ? tree.lower_bound(rp.lo)
                        : tree.begin();

                    if (rp.has_lo && !rp.lo_inclusive &&
                        it_begin != tree.end() &&
                        compare_values(it_begin->first, rp.lo, ast::BinOp::Eq)) {
                        ++it_begin;
                    }
                    auto it_end = tree.end();
                    if (rp.has_hi) {
                        it_end = rp.hi_inclusive
                            ? tree.upper_bound(rp.hi)
                            : tree.lower_bound(rp.hi);
                    }
                    for (auto it = it_begin; it != it_end; ++it) {
                        Row row;
                        if (t->read_row(it->second, row))
                            candidates.emplace_back(it->second, std::move(row));
                    }
                    used_index = true;
                }
            }
        }

        if (!used_index) {
            candidates = t->scan_all();
        }

        for (auto& [rid, row] : candidates) {
            bool match = true;
            if (s->where) {
                if (!eval_expr(s->where.get(), *t, row, match)) {
                    r.ok = false;
                    r.message = "Failed to evaluate WHERE";
                    return r;
                }
            }
            if (!match) continue;

            if (s->star) {
                r.rows.push_back(std::move(row));
            } else {
                Row out;
                out.reserve(s->columns.size());
                for (const auto& [col, _] : s->columns) {
                    int ci = schema.find_col(col);
                    out.push_back(row[static_cast<size_t>(ci)]);
                }
                r.rows.push_back(std::move(out));
            }
        }

        r.ok = true;
        if (r.rows.empty()) r.message = "Empty set";
        return r;
    }

}