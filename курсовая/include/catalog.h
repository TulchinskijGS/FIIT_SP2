#pragma once
#include "schema.h"
#include "table.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <shared_mutex>
#include <mutex>  

namespace db {

class Catalog {
public:
    explicit Catalog(std::string root);

    bool create_database(const std::string& name);
    bool drop_database(const std::string& name);
    bool use_database(const std::string& name);
    const std::string& current_db() const { return current_db_; }
    bool has_database(const std::string& name) const;

    // --- metadata: tables ---
    bool create_table(const TableSchema& schema);
    bool drop_table(const std::string& name);

    Table*       get_table(const std::string& name);
    const Table* get_table(const std::string& name) const;

    void persist(Table& t);

private:
    std::string root_;
    std::string current_db_;
    std::unordered_map<std::string, std::unique_ptr<Table>> tables_;
    mutable std::shared_mutex mtx_;

    std::string db_path(const std::string& name) const;
    std::string table_dir(const std::string& db, const std::string& table) const;
    std::string table_schema_path(const std::string& db, const std::string& table) const;

    static void save_schema(const std::string& path, const TableSchema& s);
    static bool load_schema(const std::string& path, TableSchema& out);
    void unload_tables();

    static bool valid_name(const std::string& s) {
        if (s.empty()) return false;
        if (!(std::isalpha((unsigned char)s[0]) || s[0] == '_')) return false;
        for (char c : s)
            if (!(std::isalnum((unsigned char)c) || c == '_')) return false;
        return true;
    }
};

}