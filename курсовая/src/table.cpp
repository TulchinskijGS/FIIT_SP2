#include "table.h"
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <sstream>

namespace fs = std::filesystem;

namespace db {

    Table::Table(TableSchema schema, std::string dir)
        : schema_(std::move(schema)), dir_(std::move(dir)) {
        open(dir_);
    }

    void Table::open(const std::string& dir) {
        dir_ = dir;
        fs::create_directories(dir_);
        auto pages_path = (fs::path(dir_) / "pages.db").string();
        bool create = !fs::exists(pages_path);
        pager_ = std::make_unique<Pager>(pages_path, true);
        indexes_.clear();
        for (size_t i = 0; i < schema_.columns.size(); ++i) {
            if (schema_.columns[i].indexed) {
                auto idx = std::make_unique<Index>();
                idx->col_idx = i;
                indexes_.push_back(std::move(idx));
            }
        }
        rebuild_indexes();
    }

    std::vector<uint8_t> Table::serialize(const Row& row) const {
        std::vector<uint8_t> out;
        uint16_t n = static_cast<uint16_t>(row.size());
        out.insert(out.end(), reinterpret_cast<uint8_t*>(&n), reinterpret_cast<uint8_t*>(&n) + 2);
        for (const Value& v : row) {
            if (is_null(v)) {
                out.push_back(0);
            } else if (auto p = std::get_if<int64_t>(&v)) {
                out.push_back(1);
                uint32_t len = sizeof(int64_t);
                out.insert(out.end(), reinterpret_cast<uint8_t*>(&len), reinterpret_cast<uint8_t*>(&len) + 4);
                out.insert(out.end(), reinterpret_cast<const uint8_t*>(p), reinterpret_cast<const uint8_t*>(p) + 8);
            } else {
                const std::string& s = std::get<std::string>(v);
                out.push_back(2);
                uint32_t len = static_cast<uint32_t>(s.size());
                out.insert(out.end(), reinterpret_cast<uint8_t*>(&len), reinterpret_cast<uint8_t*>(&len) + 4);
                out.insert(out.end(), s.begin(), s.end());
            }
        }
        return out;
    }

    Row Table::deserialize(const uint8_t* data, uint16_t len) const {
        Row row;
        if (len < 2) return row;
        uint16_t n;
        std::memcpy(&n, data, 2);
        size_t p = 2;
        for (uint16_t i = 0; i < n && p < len; ++i) {
            uint8_t tag = data[p++];
            if (tag == 0) { row.emplace_back(std::monostate{}); continue; }
            uint32_t l;
            std::memcpy(&l, data + p, 4); p += 4;
            if (tag == 1) {
                int64_t v;
                std::memcpy(&v, data + p, 8); p += 8;
                row.emplace_back(v);
            } else {
                std::string s(reinterpret_cast<const char*>(data + p), l); p += l;
                row.emplace_back(std::move(s));
            }
        }
        return row;
    }

    RowId Table::insert_row(const Row& row) {
        auto bytes = serialize(row);
        if (bytes.size() > PAGE_SIZE - sizeof(PageHeader) - sizeof(Slot))
            throw std::runtime_error("Row too large for page");
        for (uint32_t pid = 0; pid < pager_->page_count(); ++pid) {
            Page& p = pager_->get(pid);
            int slot = p.insert(bytes.data(), static_cast<uint16_t>(bytes.size()));
            if (slot >= 0) {
                RowId rid = make_rowid(pid, static_cast<uint16_t>(slot));
                for (auto& idx : indexes_)
                    idx->tree.insert({row[idx->col_idx], rid});
                return rid;
            }
        }
        uint32_t pid = pager_->allocate_page();
        Page& p = pager_->get(pid);
        int slot = p.insert(bytes.data(), static_cast<uint16_t>(bytes.size()));
        if (slot < 0) throw std::runtime_error("Failed to insert into fresh page");
        RowId rid = make_rowid(pid, static_cast<uint16_t>(slot));
        for (auto& idx : indexes_)
            idx->tree.insert({row[idx->col_idx], rid});
        return rid;
    }

    bool Table::read_row(RowId rid, Row& out) const {
        uint32_t pid = rowid_page(rid);
        uint16_t sid =rowid_slot(rid);
        if (pid >= pager_->page_count()) return false;
        Page& p = const_cast<Pager*>(pager_.get())->get(pid);
        const uint8_t* data;
        uint16_t len;
        if (!p.read(sid, data, len)) return false;
        out = deserialize(data, len);
        return true;
    }

    bool Table::update_row(RowId rid, const Row& new_row) {
        Row old;
        if (!read_row(rid, old)) return false;

        for (auto& idx : indexes_) idx->tree.erase(old[idx->col_idx]);

        Page& p = pager_->get(rowid_page(rid));
        p.erase(rowid_slot(rid));

        try {
            RowId new_rid = insert_row(new_row);
            (void)new_rid;
        } catch (...) {
            for (auto& idx : indexes_) idx->tree.insert({old[idx->col_idx], rid});
            return false;
        }
        return true;
    }

    bool Table::delete_row(RowId rid) {
        Row old;
        if (!read_row(rid, old)) return false;
        for (auto& idx : indexes_) idx->tree.erase(old[idx->col_idx]);
        Page& p = pager_->get(rowid_page(rid));
        return p.erase(rowid_slot(rid));
    }

    std::vector<std::pair<RowId, Row>> Table::scan_all() const {
        std::vector<std::pair<RowId, Row>> out;
        for (uint32_t pid = 0; pid < pager_->page_count(); ++pid) {
            Page& p = const_cast<Pager*>(pager_.get())->get(pid);
            for (uint16_t sid = 0; sid < p.header().slot_count; ++sid) {
                const uint8_t* data;
                uint16_t len;
                if (!p.read(sid, data, len)) continue;
                out.emplace_back(make_rowid(pid, sid), deserialize(data, len));
            }
        }
        return out;
    }

    Index* Table::find_index(size_t col_idx) {
        for (auto& i : indexes_) if (i->col_idx == col_idx) return i.get();
        return nullptr;
    }

    const Index* Table::find_index(size_t col_idx) const {
        for (auto& i : indexes_) if (i->col_idx == col_idx) return i.get();
        return nullptr;
    }

    void Table::rebuild_indexes() {
        for (auto& idx : indexes_) idx->tree.clear();
        for (uint32_t pid = 0; pid < pager_->page_count(); ++pid) {
            Page& p = pager_->get(pid);
            for (uint16_t sid = 0; sid < p.header().slot_count; ++sid) {
                const uint8_t* data;
                uint16_t len;
                if (!p.read(sid, data, len)) continue;
                Row r = deserialize(data, len);
                RowId rid = make_rowid(pid, sid);
                for (auto& idx : indexes_)
                    idx->tree.insert({r[idx->col_idx], rid});
            }
        }
    }

    void Table::flush() {
        if (pager_) pager_->flush_all();
    }

}