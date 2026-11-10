#pragma once
#include "schema.h"
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include <list>

namespace db {

    constexpr size_t PAGE_SIZE = 4096;
    constexpr uint16_t TOMBSTONE = 0xFFFF;

    struct PageHeader {
        uint32_t page_id;
        uint16_t slot_count;
        uint16_t free_space;
        uint16_t free_ptr;
        uint16_t slot_dir_end;
        uint8_t  flags;
        uint8_t  reserved[19];
    };
    static_assert(sizeof(PageHeader) == 32);

    struct Slot {
        uint16_t offset;
        uint16_t length;
    };
    static_assert(sizeof(Slot) == 4);

    inline RowId make_rowid(uint32_t page_id, uint16_t slot_id) {
        return (static_cast<uint64_t>(page_id) << 16) | slot_id;
    }
    inline uint32_t rowid_page(RowId r) { return static_cast<uint32_t>(r >> 16); }
    inline uint16_t rowid_slot(RowId r) { return static_cast<uint16_t>(r & 0xFFFF); }

    class Page {
    public:
        Page() { std::memset(data_, 0, PAGE_SIZE); }
        explicit Page(uint32_t id) {
            std::memset(data_, 0, PAGE_SIZE);
            header().page_id = id;
            header().free_space = PAGE_SIZE - sizeof(PageHeader);
            header().free_ptr = PAGE_SIZE;
            header().slot_dir_end = sizeof(PageHeader);
        }

        PageHeader& header() { return *reinterpret_cast<PageHeader*>(data_); }
        const PageHeader& header() const { return *reinterpret_cast<const PageHeader*>(data_); }

        uint8_t* raw() { return data_; }
        const uint8_t* raw() const { return data_; }

        Slot* slots() { return reinterpret_cast<Slot*>(data_ + sizeof(PageHeader)); }
        const Slot* slots() const { return reinterpret_cast<const Slot*>(data_ + sizeof(PageHeader)); }

        int insert(const void* payload, uint16_t len);

        bool erase(uint16_t slot_id);

        bool read(uint16_t slot_id, const uint8_t*& out, uint16_t& len) const;

        void compact();

        bool is_dirty() const { return header().flags & 1; }
        void mark_dirty() { header().flags |= 1; }
        void mark_clean() { header().flags &= ~1; }

    private:
        alignas(8) uint8_t data_[PAGE_SIZE];
    };

    class Pager {
    public:
        public:
        Pager(const std::string& path, bool create);
        ~Pager();

        Page& get(uint32_t page_id);
        uint32_t allocate_page();
        void free_page(uint32_t page_id);
        void flush_page(uint32_t page_id);
        void flush_all();
        uint32_t page_count() const { return page_count_; }

        void wal_append(const std::string& op);

    private:
        std::string path_;
        std::fstream file_;
        std::unordered_map<uint32_t, std::unique_ptr<Page>> cache_;
        std::list<uint32_t> lru_;
        std::unordered_map<uint32_t, std::list<uint32_t>::iterator> lru_pos_;
        std::vector<uint32_t> free_pages_;
        uint32_t page_count_ = 0;
        size_t cache_limit_ = 64;

        void evict_if_needed();
        void load_page(uint32_t page_id, Page& out);
        void write_page(uint32_t page_id, const Page& p);

        static uint32_t checksum(const uint8_t* data, size_t size, uint32_t seed);
        void wal_write_block(const std::vector<uint32_t>& dirty_pages);
        void wal_truncate();
        void wal_recover();
    };

}