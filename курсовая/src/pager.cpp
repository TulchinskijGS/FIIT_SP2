#include "pager.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

#ifdef _WIN32
#  include <io.h>
#  include <fcntl.h>
#  define DB_FSYNC(fd) _commit(fd)
#  define DB_OPEN_RD(path) _open((path), _O_RDONLY)
#  define DB_CLOSE(fd) _close(fd)
#else
#  include <fcntl.h>
#  include <unistd.h>
#  define DB_FSYNC(fd) ::fsync(fd)
#  define DB_OPEN_RD(path) ::open((path), O_RDONLY)
#  define DB_CLOSE(fd) ::close(fd)
#endif

namespace fs = std::filesystem;

namespace db {

    namespace {
        constexpr uint32_t WAL_MAGIC_BEGIN = 0x57414C31u;
        constexpr uint32_t WAL_MAGIC_END   = 0x454E4431u;
        constexpr uint32_t WAL_ENTRY_HEADER = 12;
    }

    int Page::insert(const void* payload, uint16_t len) {
        auto& h = header();
        if (h.slot_count == 0xFFFF) return -1;

        uint16_t need = static_cast<uint16_t>(len + sizeof(Slot));
        if (need > h.free_space) {
            compact();
            if (need > h.free_space) return -1;
        }

        uint16_t slot_id = h.slot_count;
        for (uint16_t i = 0; i < h.slot_count; ++i) {
            if (slots()[i].length == TOMBSTONE) { slot_id = i; break; }
        }

        uint16_t new_data_ptr = static_cast<uint16_t>(h.free_ptr - len);
        std::memcpy(data_ + new_data_ptr, payload, len);

        if (slot_id == h.slot_count) {
            slots()[slot_id] = { new_data_ptr, len };
            h.slot_dir_end = static_cast<uint16_t>(h.slot_dir_end + sizeof(Slot));
            h.slot_count   = static_cast<uint16_t>(h.slot_count + 1);
        } else {
            slots()[slot_id] = { new_data_ptr, len };
        }

        h.free_ptr   = new_data_ptr;
        h.free_space = static_cast<uint16_t>(h.free_space - need);
        mark_dirty();
        return slot_id;
    }

    bool Page::erase(uint16_t slot_id) {
        auto& h = header();
        if (slot_id >= h.slot_count) return false;
        if (slots()[slot_id].length == TOMBSTONE) return false;

        h.free_space = static_cast<uint16_t>(h.free_space + slots()[slot_id].length);
        slots()[slot_id].length = TOMBSTONE;
        slots()[slot_id].offset = 0;
        mark_dirty();
        return true;
    }

    bool Page::read(uint16_t slot_id, const uint8_t*& out, uint16_t& len) const {
        const auto& h = header();
        if (slot_id >= h.slot_count) return false;
        const Slot& s = slots()[slot_id];
        if (s.length == TOMBSTONE) return false;
        out = data_ + s.offset;
        len = s.length;
        return true;
    }

    void Page::compact() {
        auto& h = header();

        std::vector<uint8_t> tmp;
        tmp.reserve(PAGE_SIZE);

        std::vector<Slot> new_slots;
        new_slots.reserve(h.slot_count);

        for (uint16_t i = 0; i < h.slot_count; ++i) {
            const Slot& s = slots()[i];
            if (s.length == TOMBSTONE) {
                new_slots.push_back({0, TOMBSTONE});
                continue;
            }
            uint16_t new_off = static_cast<uint16_t>(PAGE_SIZE - tmp.size() - s.length);
            tmp.insert(tmp.end(), data_ + s.offset, data_ + s.offset + s.length);
            new_slots.push_back({new_off, s.length});
        }

        uint16_t ptr = PAGE_SIZE;
        for (size_t i = 0; i < new_slots.size(); ++i) {
            const Slot& s = slots()[i];
            if (s.length == TOMBSTONE) continue;
            ptr = static_cast<uint16_t>(ptr - s.length);
            std::memmove(data_ + ptr, data_ + s.offset, s.length);
            new_slots[i].offset = ptr;
        }

        for (size_t i = 0; i < new_slots.size(); ++i)
            slots()[i] = new_slots[i];

        h.free_ptr   = ptr;
        h.free_space = static_cast<uint16_t>(ptr - h.slot_dir_end);
        mark_dirty();
    }

    Pager::Pager(const std::string& path, bool create) : path_(path) {
        if (create && !fs::exists(path_)) {
            std::ofstream f(path_, std::ios::binary);
        }

        file_.open(path_, std::ios::in | std::ios::out | std::ios::binary);
        if (!file_) throw std::runtime_error("Cannot open pager file: " + path_);

        wal_recover();

        file_.seekg(0, std::ios::end);
        auto sz = file_.tellg();
        page_count_ = sz > 0 ? static_cast<uint32_t>(sz) / PAGE_SIZE : 0;

        auto fl = path_ + ".free";
        std::ifstream ff(fl);
        if (ff) {
            uint32_t id;
            while (ff >> id) free_pages_.push_back(id);
        }
    }

    Pager::~Pager() {
        try {
            flush_all();
        } catch (...) {

        }

        std::ofstream ff(path_ + ".free", std::ios::trunc);
        if (ff) {
            for (uint32_t id : free_pages_) ff << id << '\n';
        }
    }


    void Pager::load_page(uint32_t page_id, Page& out) {
        file_.seekg(static_cast<std::streamoff>(page_id) * PAGE_SIZE);
        file_.read(reinterpret_cast<char*>(out.raw()), PAGE_SIZE);
        if (file_.gcount() != static_cast<std::streamsize>(PAGE_SIZE)) {
            out = Page(page_id);
        }
    }

    void Pager::write_page(uint32_t page_id, const Page& p) {
        file_.seekp(static_cast<std::streamoff>(page_id) * PAGE_SIZE);
        file_.write(reinterpret_cast<const char*>(p.raw()), PAGE_SIZE);
        file_.flush();
    }


    Page& Pager::get(uint32_t page_id) {
        auto it = cache_.find(page_id);
        if (it != cache_.end()) {
            auto pos = lru_pos_.find(page_id);
            if (pos != lru_pos_.end()) lru_.erase(pos->second);
            lru_.push_front(page_id);
            lru_pos_[page_id] = lru_.begin();
            return *it->second;
        }

        evict_if_needed();

        auto p = std::make_unique<Page>();
        load_page(page_id, *p);
        Page* raw = p.get();

        cache_[page_id] = std::move(p);
        lru_.push_front(page_id);
        lru_pos_[page_id] = lru_.begin();
        return *raw;
    }

    void Pager::evict_if_needed() {
        while (cache_.size() >= cache_limit_ && !lru_.empty()) {
            uint32_t victim = lru_.back();
            lru_.pop_back();
            lru_pos_.erase(victim);

            auto it = cache_.find(victim);
            if (it == cache_.end()) continue;

            if (it->second->is_dirty()) {
                wal_write_block({victim});
                write_page(victim, *it->second);
                it->second->mark_clean();
                wal_truncate();
            }
            cache_.erase(it);
        }
    }


    uint32_t Pager::allocate_page() {
        uint32_t id;
        if (!free_pages_.empty()) {
            id = free_pages_.back();
            free_pages_.pop_back();
        } else {
            id = page_count_++;
        }

        cache_[id] = std::make_unique<Page>(id);
        cache_[id]->mark_dirty();

        auto pos = lru_pos_.find(id);
        if (pos != lru_pos_.end()) lru_.erase(pos->second);
        lru_.push_front(id);
        lru_pos_[id] = lru_.begin();

        evict_if_needed();
        return id;
    }

    void Pager::free_page(uint32_t page_id) {
        cache_.erase(page_id);
        auto pos = lru_pos_.find(page_id);
        if (pos != lru_pos_.end()) {
            lru_.erase(pos->second);
            lru_pos_.erase(pos);
        }
        free_pages_.push_back(page_id);
    }


    uint32_t Pager::checksum(const uint8_t* data, size_t size, uint32_t seed) {
        uint32_t h = seed ^ 0x9E3779B9u;
        for (size_t i = 0; i < size; ++i) {
            h ^= data[i];
            h = (h << 5) | (h >> 27);
            h *= 0x85EBCA6Bu;
        }
        return h;
    }

    void Pager::wal_write_block(const std::vector<uint32_t>& dirty_pages) {
        if (dirty_pages.empty()) return;

        std::vector<uint8_t> buf;
        buf.reserve(8 + dirty_pages.size() * (WAL_ENTRY_HEADER + PAGE_SIZE) + 4);

        auto push_u32 = [&](uint32_t v) {
            uint8_t b[4];
            std::memcpy(b, &v, 4);
            buf.insert(buf.end(), b, b + 4);
        };

        push_u32(WAL_MAGIC_BEGIN);
        push_u32(static_cast<uint32_t>(dirty_pages.size()));

        for (uint32_t pid : dirty_pages) {
            auto it = cache_.find(pid);
            if (it == cache_.end()) continue;
            const Page& p = *it->second;

            push_u32(pid);
            push_u32(static_cast<uint32_t>(PAGE_SIZE));
            push_u32(checksum(p.raw(), PAGE_SIZE, pid));
            buf.insert(buf.end(), p.raw(), p.raw() + PAGE_SIZE);
        }

        push_u32(WAL_MAGIC_END);

        std::string wal_path = path_ + ".wal";

        // Пишем одним куском + fsync на самом файле.
        {
            std::ofstream f(wal_path, std::ios::binary | std::ios::trunc);
            if (!f) return;
            f.write(reinterpret_cast<const char*>(buf.data()),
                    static_cast<std::streamsize>(buf.size()));
            f.flush();
        }

        int fd = DB_OPEN_RD(wal_path.c_str());
        if (fd >= 0) {
            DB_FSYNC(fd);
            DB_CLOSE(fd);
        }
    }

    void Pager::wal_truncate() {
        std::string wal_path = path_ + ".wal";
        std::error_code ec;
        fs::remove(wal_path, ec);
    }

    void Pager::wal_recover() {
        std::string wal_path = path_ + ".wal";

        std::ifstream f(wal_path, std::ios::binary);
        if (!f) return;

        std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
        f.close();

        auto drop_wal = [&]() {
            std::error_code ec;
            fs::remove(wal_path, ec);
        };

        if (buf.size() < 8) {
            drop_wal();
            return;
        }

        auto read_u32 = [&](size_t off) -> uint32_t {
            uint32_t v = 0;
            std::memcpy(&v, buf.data() + off, 4);
            return v;
        };

        if (read_u32(0) != WAL_MAGIC_BEGIN) {
            drop_wal();
            return;
        }

        uint32_t count = read_u32(4);
        size_t off = 8;
        size_t need = 8 + static_cast<size_t>(count) * (WAL_ENTRY_HEADER + PAGE_SIZE) + 4;

        if (buf.size() < need) {
            drop_wal();
            return;
        }

        uint32_t end_magic = read_u32(off + static_cast<size_t>(count) * (WAL_ENTRY_HEADER + PAGE_SIZE));
        if (end_magic != WAL_MAGIC_END) {
            drop_wal();
            return;
        }

        // Применяем записи.
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t pid  = read_u32(off);
            uint32_t size = read_u32(off + 4);
            uint32_t cs   = read_u32(off + 8);
            const uint8_t* data = buf.data() + off + WAL_ENTRY_HEADER;

            if (size != PAGE_SIZE) {
                off += WAL_ENTRY_HEADER + size;
                continue;
            }

            if (checksum(data, PAGE_SIZE, pid) != cs) {
                off += WAL_ENTRY_HEADER + PAGE_SIZE;
                continue;
            }

            file_.seekp(static_cast<std::streamoff>(pid) * PAGE_SIZE);
            file_.write(reinterpret_cast<const char*>(data), PAGE_SIZE);

            if (pid >= page_count_) page_count_ = pid + 1;
            off += WAL_ENTRY_HEADER + PAGE_SIZE;
        }

        file_.flush();

        file_.seekg(0, std::ios::end);
        auto sz = file_.tellg();
        if (sz > 0) page_count_ = static_cast<uint32_t>(sz) / PAGE_SIZE;

        drop_wal();
    }

    void Pager::flush_page(uint32_t page_id) {
        auto it = cache_.find(page_id);
        if (it == cache_.end()) return;
        if (!it->second->is_dirty()) return;

        wal_write_block({page_id});
        write_page(page_id, *it->second);
        it->second->mark_clean();
        wal_truncate();
    }

    void Pager::flush_all() {
        std::vector<uint32_t> dirty;
        dirty.reserve(cache_.size());
        for (auto& [id, p] : cache_) {
            if (p->is_dirty()) dirty.push_back(id);
        }
        if (dirty.empty()) return;

        wal_write_block(dirty);

        for (uint32_t id : dirty) {
            auto it = cache_.find(id);
            if (it == cache_.end()) continue;
            write_page(id, *it->second);
            it->second->mark_clean();
        }
        file_.flush();

        wal_truncate();
    }

    void Pager::wal_append(const std::string& op) {
        std::ofstream f(path_ + ".wal.log", std::ios::app);
        if (f) f << op << '\n';
    }

}