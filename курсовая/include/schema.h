#pragma once
#include "value.h"
#include <vector>
#include <string>
#include <unordered_map>

namespace db {

    enum class ColType { Int, String };

    struct ColumnDef {
        std::string name;
        ColType type;
        bool not_null = false;
        bool indexed = false;
    };

    struct TableSchema {
        std::string name;
        std::vector<ColumnDef> columns;
        std::unordered_map<std::string, size_t> name_to_idx;

        int find_col(const std::string& n) const {
            auto it = name_to_idx.find(n);
            return it == name_to_idx.end() ? -1 : static_cast<int>(it->second);
        }
    };

    using RowId = uint64_t;
    using Row   = std::vector<Value>;

}