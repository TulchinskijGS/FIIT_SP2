#include "catalog.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace db {

    Catalog::Catalog(std::string root) : root_(std::move(root)) {
        std::error_code ec;
        fs::create_directories(root_, ec);
        if (ec) throw std::runtime_error("Cannot create data root: " + root_ + " (" + ec.message() + ")");
    }

    std::string Catalog::db_path(const std::string& name) const {
        return (fs::path(root_) / name).string();
    }

    std::string Catalog::table_dir(const std::string& db, const std::string& table) const {
        return (fs::path(root_) / db / table).string();
    }

    std::string Catalog::table_schema_path(const std::string& db, const std::string& table) const {
        return (fs::path(table_dir(db, table)) / "schema.txt").string();
    }

    void Catalog::save_schema(const std::string& path, const TableSchema& s) {
        std::ofstream f(path, std::ios::trunc);
        if (!f) throw std::runtime_error("Cannot write schema: " + path);
        f << s.name << '\n';
        for (const auto& c : s.columns) {
            f << c.name << ' '
            << static_cast<int>(c.type) << ' '
            << (c.not_null ? 1 : 0) << ' '
            << (c.indexed  ? 1 : 0) << '\n';
        }
    }

    bool Catalog::load_schema(const std::string& path, TableSchema& out) {
        std::ifstream f(path);
        if (!f) return false;

        TableSchema s;
        if (!std::getline(f, s.name)) return false;
        if (s.name.empty()) return false;

        std::string line;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            std::istringstream ss(line);
            ColumnDef c;
            int t = 0, nn = 0, ix = 0;
            if (!(ss >> c.name >> t >> nn >> ix)) return false;
            c.type     = static_cast<ColType>(t);
            c.not_null = (nn != 0);
            c.indexed  = (ix != 0);

            if (s.name_to_idx.count(c.name)) return false;
            s.name_to_idx[c.name] = s.columns.size();
            s.columns.push_back(c);
        }

        out = std::move(s);
        return true;
    }

    bool Catalog::has_database(const std::string& name) const {
        std::error_code ec;
        return fs::exists(db_path(name), ec) && fs::is_directory(db_path(name), ec);
    }

    bool Catalog::create_database(const std::string& name) {
        if (!valid_name(name)) return false;

        std::unique_lock lock(mtx_);

        if (name.empty()) return false;
        auto p = db_path(name);

        std::error_code ec;
        if (fs::exists(p, ec)) return false;

        fs::create_directories(p, ec);
        return !ec;
    }

    bool Catalog::drop_database(const std::string& name) {
        std::unique_lock lock(mtx_);

        auto p = db_path(name);
        std::error_code ec;
        if (!fs::exists(p, ec)) return false;

        if (current_db_ == name) {
            unload_tables();
            current_db_.clear();
        }

        fs::remove_all(p, ec);
        return !ec;
    }

    bool Catalog::use_database(const std::string& name) {
        if (!valid_name(name)) return false;

        std::unique_lock lock(mtx_);

        auto p = db_path(name);
        std::error_code ec;
        if (!fs::exists(p, ec) || !fs::is_directory(p, ec)) return false;

        unload_tables();
        current_db_ = name;

        for (auto& entry : fs::directory_iterator(p, ec)) {
            if (ec) break;
            if (!entry.is_directory()) continue;

            auto schema_path = (entry.path() / "schema.txt").string();
            TableSchema schema;
            if (!load_schema(schema_path, schema)) continue;

            try {
                auto tbl = std::make_unique<Table>(schema, entry.path().string());
                tables_[schema.name] = std::move(tbl);
            } catch (const std::exception&) {
            }
        }
        return true;
    }

    void Catalog::unload_tables() {
        for (auto& [name, tbl] : tables_) {
            if (tbl) {
                try { tbl->flush(); } catch (...) {  }
            }
        }
        tables_.clear();
    }

    bool Catalog::create_table(const TableSchema& schema) {
        if (!valid_name(schema.name)) return false;

        std::unique_lock lock(mtx_);

        if (current_db_.empty()) return false;
        if (schema.name.empty()) return false;
        if (tables_.count(schema.name)) return false;

        auto dir = table_dir(current_db_, schema.name);
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec) return false;

        try {
            save_schema(table_schema_path(current_db_, schema.name), schema);
            auto tbl = std::make_unique<Table>(schema, dir);
            tables_[schema.name] = std::move(tbl);
        } catch (const std::exception&) {
            fs::remove_all(dir, ec);
            return false;
        }
        return true;
    }

    bool Catalog::drop_table(const std::string& name) {
        std::unique_lock lock(mtx_);

        auto it = tables_.find(name);
        if (it == tables_.end()) return false;

        try { it->second->flush(); } catch (...) {  }
        tables_.erase(it);

        if (current_db_.empty()) return true;
        auto dir = table_dir(current_db_, name);
        std::error_code ec;
        fs::remove_all(dir, ec);
        return !ec;
    }

    Table* Catalog::get_table(const std::string& name) {
        std::shared_lock lock(mtx_);
        auto it = tables_.find(name);
        return it == tables_.end() ? nullptr : it->second.get();
    }

    const Table* Catalog::get_table(const std::string& name) const {
        std::shared_lock lock(mtx_);
        auto it = tables_.find(name);
        return it == tables_.end() ? nullptr : it->second.get();
    }

    void Catalog::persist(Table& t) {
        t.flush();
    }

}