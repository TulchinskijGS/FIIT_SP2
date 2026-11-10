#include "../include/allocator_boundary_tags.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>

namespace
{
    struct allocator_metadata
    {
        std::pmr::memory_resource* parent;
        allocator_with_fit_mode::fit_mode mode;
        size_t size;
        std::mutex* mutex;
        void* first_block;
    };

    struct block_metadata
    {
        size_t size;
        void* previous;
        void* next;
        void* end;
    };

    constexpr size_t occupied_mask = size_t(1) << (sizeof(size_t) * 8 - 1);

    constexpr size_t metadata_size = sizeof(size_t) + sizeof(void*) * 3;

    static_assert(sizeof(block_metadata) == metadata_size, "block_metadata layout differs from the expected test layout");

    size_t block_size(const block_metadata* block) noexcept
    {
        return block->size & ~occupied_mask;
    }

    bool is_occupied(const block_metadata* block) noexcept
    {
        return block != nullptr && (block->size & occupied_mask) != 0;
    }

    void mark_occupied(block_metadata* block) noexcept
    {
        block->size |= occupied_mask;
    }

    void mark_free(block_metadata* block) noexcept
    {
        block->size &= ~occupied_mask;
    }

    void set_block_size(block_metadata* block, size_t size) noexcept
    {
        const bool occupied = is_occupied(block);
        block->size = size;
        if (occupied)
        {
            mark_occupied(block);
        }
    }

    char* block_begin(block_metadata* block) noexcept
    {
        return reinterpret_cast<char*>(block);
    }

    char const* block_begin(block_metadata const* block) noexcept
    {
        return reinterpret_cast<char const*>(block);
    }

    void* user_pointer(block_metadata* block) noexcept
    {
        return block_begin(block) + metadata_size;
    }

    block_metadata* metadata_from_user(void* ptr) noexcept
    {
        return reinterpret_cast<block_metadata*>(static_cast<char*>(ptr) - metadata_size);
    }

    void update_end_tag(block_metadata* block) noexcept
    {
        if (block == nullptr)
        {
            return;
        }
        if (block_size(block) == 0)
        {
            block->end = block;
        }
        else
        {
            block->end = block_begin(block) + metadata_size + block_size(block) - 1;
        }
    }

    bool is_inside(allocator_metadata const* meta, block_metadata const* block) noexcept
    {
        if (meta == nullptr || block == nullptr)
        {
            return false;
        }
        const auto* begin = static_cast<char const*>(meta->first_block);
        if (begin == nullptr)
        {
            return false;
        }
        const auto* pool_begin = reinterpret_cast<char const*>(meta->first_block);
        const auto* pool_end = pool_begin + meta->size;
        const auto* address = reinterpret_cast<char const*>(block);
        return address >= pool_begin && address < pool_end;
    }

    block_metadata* find_block(allocator_metadata* meta, void* user_ptr) noexcept
    {
        if (meta == nullptr || user_ptr == nullptr)
        {
            return nullptr;
        }
        auto* current = reinterpret_cast<block_metadata*>(meta->first_block);
        while (current != nullptr)
        {
            if (user_pointer(current) == user_ptr)
            {
                return current;
            }
            current = reinterpret_cast<block_metadata*>(current->next);
        }
        return nullptr;
    }

    bool can_split(
        block_metadata const* block,
        size_t requested_size) noexcept
    {
        const size_t current_size = block_size(block);
        if (current_size < requested_size)
        {
            return false;
        }
        const size_t remaining = current_size - requested_size;
        return remaining > metadata_size;
    }

    block_metadata* next_block(block_metadata* block) noexcept
    {
        if (block == nullptr)
        {
            return nullptr;
        }
        return reinterpret_cast<block_metadata*>(block->next);
    }

    block_metadata const* next_block(block_metadata const* block) noexcept
    {
        if (block == nullptr)
        {
            return nullptr;
        }
        return reinterpret_cast<block_metadata const*>(block->next);
    }

    block_metadata* previous_block(block_metadata* block) noexcept
    {
        if (block == nullptr)
        {
            return nullptr;
        }
        return reinterpret_cast<block_metadata*>(block->previous);
    }

    block_metadata const* previous_block(block_metadata const* block) noexcept
    {
        if (block == nullptr)
        {
            return nullptr;
        }
        return reinterpret_cast<block_metadata const*>(
            block->previous);
    }

    void merge_with_next(block_metadata* block) noexcept
    {
        if (block == nullptr)
        {
            return;
        }
        auto* next = next_block(block);
        if (next == nullptr || is_occupied(block) || is_occupied(next))
        {
            return;
        }
        const size_t new_size = block_size(block) + metadata_size + block_size(next);
        block->size = new_size;
        mark_free(block);
        block->next = next->next;
        if (next->next != nullptr)
        {
            auto* after = reinterpret_cast<block_metadata*>(next->next);
            after->previous = block;
        }
        update_end_tag(block);
    }
    block_metadata* select_block(allocator_metadata* meta, size_t requested_size) noexcept
    {
        block_metadata* selected = nullptr;
        auto* current = reinterpret_cast<block_metadata*>(meta->first_block);
        while (current != nullptr)
        {
            if (!is_occupied(current) && block_size(current) >= requested_size)
            {
                if (selected == nullptr)
                {
                    selected = current;
                    if (meta->mode == allocator_with_fit_mode::fit_mode::first_fit)
                    {
                        return selected;
                    }
                }
                else
                {
                    const size_t current_size = block_size(current);
                    const size_t selected_size = block_size(selected);
                    if (meta->mode == allocator_with_fit_mode::fit_mode::the_best_fit && current_size < selected_size)
                    {
                        selected = current;
                    }
                    if (meta->mode == allocator_with_fit_mode::fit_mode::the_worst_fit && current_size > selected_size)
                    {
                        selected = current;
                    }
                }
            }
            current = next_block(current);
        }
        return selected;
    }

    bool belongs_to_allocator(allocator_metadata* meta, void* ptr) noexcept
    {
        if (meta == nullptr || ptr == nullptr)
        {
            return false;
        }
        return find_block(meta, ptr) != nullptr;
    }
}

allocator_boundary_tags::allocator_boundary_tags(size_t space_size, std::pmr::memory_resource* parent_allocator, allocator_with_fit_mode::fit_mode allocate_fit_mode) : _trusted_memory(nullptr)
{
    if (space_size < metadata_size)
    {
        throw std::bad_alloc();
    }
    if (parent_allocator == nullptr)
    {
        parent_allocator = std::pmr::get_default_resource();
    }
    auto* meta = new allocator_metadata{};
    try
    {
        meta->parent = parent_allocator;
        meta->mode = allocate_fit_mode;
        meta->size = space_size;
        meta->mutex = nullptr;
        meta->first_block = nullptr;
        _trusted_memory = parent_allocator->allocate(space_size, alignof(std::max_align_t));
        meta->mutex = new std::mutex;
        auto* first = reinterpret_cast<block_metadata*>(_trusted_memory);
        first->size = space_size - metadata_size;
        first->previous = nullptr;
        first->next = nullptr;
        meta->first_block = first;
        update_end_tag(first);
        auto* old_memory = _trusted_memory;
        auto* external_meta = new allocator_metadata(*meta);
        external_meta->first_block = old_memory;
        delete meta;
        meta = external_meta;
        _trusted_memory = meta;
    }
    catch (...)
    {
        if (meta != nullptr)
        {
            delete meta->mutex;
            if (_trusted_memory != nullptr)
            {
                parent_allocator->deallocate(_trusted_memory, space_size, alignof(std::max_align_t));
                _trusted_memory = nullptr;
            }
            delete meta;
        }
        throw;
    }
}

allocator_boundary_tags::~allocator_boundary_tags()
{
    auto* meta = reinterpret_cast<allocator_metadata*>( _trusted_memory);
    if (meta == nullptr)
    {
        return;
    }
    auto* pool = meta->first_block;
    auto* parent = meta->parent;
    const size_t size = meta->size;
    if (meta->mutex != nullptr)
    {
        delete meta->mutex;
        meta->mutex = nullptr;
    }
    if (pool != nullptr && parent != nullptr)
    {
        parent->deallocate(pool, size, alignof(std::max_align_t));
    }
    delete meta;
    _trusted_memory = nullptr;
}

allocator_boundary_tags::allocator_boundary_tags(allocator_boundary_tags const& other) : _trusted_memory(nullptr)
{
    auto* other_meta = reinterpret_cast<allocator_metadata*>(other._trusted_memory);
    if (other_meta == nullptr)
    {
        return;
    }
    std::lock_guard lock(*other_meta->mutex);
    auto* new_meta = new allocator_metadata{};
    try
    {
        new_meta->parent = other_meta->parent;
        new_meta->mode = other_meta->mode;
        new_meta->size = other_meta->size;
        new_meta->mutex = new std::mutex;
        new_meta->first_block = nullptr;
        void* new_pool = new_meta->parent->allocate(new_meta->size, alignof(std::max_align_t));
        std::memcpy(new_pool, other_meta->first_block, new_meta->size);
        const auto old_base = reinterpret_cast<std::uintptr_t>(other_meta->first_block);
        const auto new_base = reinterpret_cast<std::uintptr_t>(new_pool);
        const auto delta = static_cast<std::intptr_t>(new_base - old_base);
        auto* current = reinterpret_cast<block_metadata*>(new_pool);
        while (current != nullptr)
        {
            if (current->previous != nullptr)
            {
                current->previous = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(current->previous) + delta);
            }
            if (current->next != nullptr)
            {
                current->next = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(current->next) + delta);
            }
            if (current->end != nullptr)
            {
                current->end = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(current->end) + delta);
            }
            auto* next = reinterpret_cast<block_metadata*>(current->next);
            current = next;
        }
        new_meta->first_block = new_pool;
        _trusted_memory = new_meta;
    }
    catch (...)
    {
        delete new_meta->mutex;
        delete new_meta;
        throw;
    }
}

allocator_boundary_tags& allocator_boundary_tags::operator=(allocator_boundary_tags const& other)
{
    if (this == &other)
    {
        return *this;
    }
    this->~allocator_boundary_tags();
    new (this)allocator_boundary_tags(other);
    return *this;
}

allocator_boundary_tags::allocator_boundary_tags(allocator_boundary_tags&& other) noexcept : _trusted_memory(other._trusted_memory)
{
    other._trusted_memory = nullptr;
}

allocator_boundary_tags& allocator_boundary_tags::operator=(allocator_boundary_tags&& other) noexcept
{
    if (this == &other)
    {
        return *this;
    }
    this->~allocator_boundary_tags();
    _trusted_memory = other._trusted_memory;
    other._trusted_memory = nullptr;
    return *this;
}

void* allocator_boundary_tags::do_allocate_sm(size_t bytes)
{
    auto* meta = reinterpret_cast<allocator_metadata*>(_trusted_memory);
    if (meta == nullptr)
    {
        throw std::bad_alloc();
    }
    std::lock_guard lock(*meta->mutex);
    const size_t requested_size = bytes;
    auto* selected = select_block(meta, requested_size);
    if (selected == nullptr)
    {
        throw std::bad_alloc();
    }
    const size_t old_size = block_size(selected);
    if (can_split(selected, requested_size))
    {
        auto* new_block = reinterpret_cast<block_metadata*>(block_begin(selected) + metadata_size + requested_size);
        const size_t remaining_size = old_size - requested_size - metadata_size;
        new_block->size = remaining_size;
        mark_free(new_block);
        new_block->previous = selected;
        new_block->next = selected->next;
        if (selected->next != nullptr)
        {
            auto* next = reinterpret_cast<block_metadata*>(selected->next);
            next->previous = new_block;
        }
        selected->next = new_block;
        selected->size = requested_size;
        mark_occupied(selected);
        update_end_tag(selected);
        update_end_tag(new_block);
    }
    else
    {
        mark_occupied(selected);
        update_end_tag(selected);
    }
    return user_pointer(selected);
}

void allocator_boundary_tags::do_deallocate_sm(void* at) {
    if (at == nullptr)
    {
        return;
    }
    auto* meta = reinterpret_cast<allocator_metadata*>(_trusted_memory);
    if (meta == nullptr)
    {
        throw std::bad_alloc();
    }
    std::lock_guard lock(*meta->mutex);
    auto* target = find_block(meta, at);
    if (target == nullptr)
    {
        throw std::bad_alloc();
    }
    if (!is_occupied(target))
    {
        throw std::bad_alloc();
    }
    mark_free(target);
    auto* previous = previous_block(target);
    if (previous != nullptr && !is_occupied(previous))
    {
        merge_with_next(previous);
        target = previous;
    }

    auto* next = next_block(target);
    if (next != nullptr && !is_occupied(next))
    {
        merge_with_next(target);
    }
    update_end_tag(target);
}

bool allocator_boundary_tags::do_is_equal(const std::pmr::memory_resource& other) const noexcept
{
    return dynamic_cast<const allocator_boundary_tags*>(&other) != nullptr;
}

void allocator_boundary_tags::set_fit_mode(allocator_with_fit_mode::fit_mode mode)
{
    auto* meta = reinterpret_cast<allocator_metadata*>(_trusted_memory);
    if (meta == nullptr)
    {
        return;
    }
    std::lock_guard lock(*meta->mutex);
    meta->mode = mode;
}

std::vector<allocator_test_utils::block_info> allocator_boundary_tags::get_blocks_info() const
{
    if (_trusted_memory == nullptr)
    {
        return {};
    }
    auto* meta = reinterpret_cast<allocator_metadata*>(_trusted_memory);
    std::lock_guard lock(*meta->mutex);
    return get_blocks_info_inner();
}

std::vector<allocator_test_utils::block_info> allocator_boundary_tags::get_blocks_info_inner() const
{
    std::vector<allocator_test_utils::block_info> result;
    auto* meta = reinterpret_cast<allocator_metadata*>(_trusted_memory);
    if (meta == nullptr || meta->first_block == nullptr)
    {
        return result;
    }
    auto* current = reinterpret_cast<block_metadata*>(meta->first_block);
    while (current != nullptr)
    {
        result.push_back({block_size(current) + metadata_size, is_occupied(current)});
        current = next_block(current);
    }
    return result;
}

allocator_boundary_tags::boundary_iterator::boundary_iterator() : _occupied_ptr(nullptr), _occupied(false), _trusted_memory(nullptr)
{
}

allocator_boundary_tags::boundary_iterator::boundary_iterator(void* trusted) : _occupied_ptr(nullptr), _occupied(false), _trusted_memory(trusted)
{
    if (trusted == nullptr)
    {
        return;
    }
    auto* meta = reinterpret_cast<allocator_metadata*>(trusted);
    if (meta == nullptr)
    {
        return;
    }
    _occupied_ptr = meta->first_block;
    if (_occupied_ptr != nullptr)
    {
        auto* block = reinterpret_cast<block_metadata*>(_occupied_ptr);
        _occupied = is_occupied(block);
    }
}

bool allocator_boundary_tags::boundary_iterator::operator==(const boundary_iterator& other) const noexcept
{
    return _occupied_ptr == other._occupied_ptr &&
           _trusted_memory == other._trusted_memory;
}

bool allocator_boundary_tags::boundary_iterator::operator!=(const boundary_iterator& other) const noexcept
{
    return !(*this == other);
}

allocator_boundary_tags::boundary_iterator& allocator_boundary_tags::boundary_iterator::operator++() &
noexcept
{
    if (_occupied_ptr == nullptr)
    {
        return *this;
    }
    auto* current = reinterpret_cast<block_metadata*>(_occupied_ptr);
    _occupied_ptr = current->next;
    if (_occupied_ptr != nullptr)
    {
        auto* next = reinterpret_cast<block_metadata*>(_occupied_ptr);
        _occupied = is_occupied(next);
    }
    else
    {
        _occupied = false;
    }
    return *this;
}

allocator_boundary_tags::boundary_iterator& allocator_boundary_tags::boundary_iterator::operator--() &
noexcept
{
    if (_occupied_ptr == nullptr)
    {
        auto* meta = reinterpret_cast<allocator_metadata*>(_trusted_memory);
        if (meta == nullptr || meta->first_block == nullptr)
        {
            return *this;
        }
        auto* current = reinterpret_cast<block_metadata*>(meta->first_block);
        while (current->next != nullptr)
        {
            current = reinterpret_cast<block_metadata*>(current->next);
        }
        _occupied_ptr = current;
        _occupied = is_occupied(current);
        return *this;
    }
    auto* current = reinterpret_cast<block_metadata*>(_occupied_ptr);
    _occupied_ptr = current->previous;
    if (_occupied_ptr != nullptr)
    {
        auto* previous = reinterpret_cast<block_metadata*>(_occupied_ptr);
        _occupied = is_occupied(previous);
    }
    else
    {
        _occupied = false;
    }
    return *this;
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::boundary_iterator::operator++(int)
{
    boundary_iterator copy(*this);
    ++(*this);
    return copy;
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::boundary_iterator::operator--(int)
{
    boundary_iterator copy(*this);
    --(*this);
    return copy;
}

size_t allocator_boundary_tags::boundary_iterator::size() const noexcept
{
    if (_occupied_ptr == nullptr)
    {
        return 0;
    }
    auto* block = reinterpret_cast<block_metadata*>(_occupied_ptr);
    return block_size(block) + metadata_size;
}

bool allocator_boundary_tags::boundary_iterator::occupied() const noexcept
{
    return _occupied;
}

void* allocator_boundary_tags::boundary_iterator::operator*() const noexcept
{
    if (_occupied_ptr == nullptr)
    {
        return nullptr;
    }
    auto* block = reinterpret_cast<block_metadata*>(_occupied_ptr);
    return user_pointer(block);
}

void* allocator_boundary_tags::boundary_iterator::get_ptr() const noexcept
{
    return _occupied_ptr;
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::begin() const noexcept
{
    boundary_iterator result;
    result._trusted_memory = _trusted_memory;
    auto* meta = reinterpret_cast<allocator_metadata*>(_trusted_memory);
    if (meta != nullptr)
    {
        result._occupied_ptr = meta->first_block;
            if (result._occupied_ptr != nullptr)
        {
            auto* block = reinterpret_cast<block_metadata*>(result._occupied_ptr);
            result._occupied = is_occupied(block);
        }
    }
    return result;
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::end() const noexcept
{
    boundary_iterator result;

    result._trusted_memory =
        _trusted_memory;

    result._occupied_ptr = nullptr;
    result._occupied = false;

    return result;
}