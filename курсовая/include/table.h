#pragma once
#include "pager.h"
#include "schema.h"
#include "b_tree.h"
#include <memory>
#include <vector>

namespace db {

struct Index {
    using Tree = B_tree<Value, RowId, ValueLess, 5>;
    Tree tree;
    size_t col_idx = 0;
};

class Table {
public:
    Table() = default;
    Table(TableSchema schema, std::string dir);

    void open(const std::string& dir);

    RowId insert_row(const Row& row);
    bool    update_row(RowId rid, const Row& new_row);
    bool    delete_row(RowId rid);
    bool    read_row(RowId rid, Row& out) const;

    std::vector<std::pair<RowId, Row>> scan_all() const;

    // Индексы
    Index* find_index(size_t col_idx);
    const Index* find_index(size_t col_idx) const;

    const TableSchema& schema() const { return schema_; }
    void set_schema(TableSchema s) { schema_ = std::move(s); }

    void flush();

private:
    TableSchema schema_;
    std::string dir_;
    std::unique_ptr<Pager> pager_;
    std::vector<std::unique_ptr<Index>> indexes_;

    std::vector<uint8_t> serialize(const Row& row) const;
    Row deserialize(const uint8_t* data, uint16_t len) const;
    void rebuild_indexes();
};

}