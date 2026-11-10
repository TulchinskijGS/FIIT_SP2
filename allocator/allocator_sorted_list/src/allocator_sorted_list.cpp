#include "../include/allocator_sorted_list.h"

#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>

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
        void* next;
        size_t size;
    };

    constexpr size_t occupied_mask =
        size_t(1) << (sizeof(size_t) * 8 - 1);

    bool is_occupied(const block_metadata* block)
    {
        return block != nullptr && (block->size & occupied_mask) != 0;
    }

    size_t get_block_size(const block_metadata* block)
    {
        return block->size & ~occupied_mask;
    }

    void set_occupied(block_metadata* block)
    {
        block->size |= occupied_mask;
    }

    void set_free(block_metadata* block)
    {
        block->size &= ~occupied_mask;
    }
}

allocator_sorted_list::allocator_sorted_list(size_t space_size, std::pmr::memory_resource* parent_allocator, allocator_with_fit_mode::fit_mode allocate_fit_mode)
{
    if (space_size <= allocator_metadata_size + block_metadata_size)
    {
        throw std::bad_alloc();
    }
    if (parent_allocator == nullptr)
    {
        parent_allocator = std::pmr::new_delete_resource();
    }
    _trusted_memory = parent_allocator->allocate(space_size, alignof(std::max_align_t));
    auto* meta = reinterpret_cast<allocator_metadata*>(_trusted_memory);
    meta->parent = parent_allocator;
    meta->mode = allocate_fit_mode;
    meta->size = space_size;
    meta->mutex = new(static_cast<char*>(_trusted_memory) + sizeof(allocator_metadata))std::mutex;
    auto* first = reinterpret_cast<block_metadata*>(static_cast<char*>(_trusted_memory) + allocator_metadata_size);
    first->next = nullptr;
    first->size = space_size - allocator_metadata_size - block_metadata_size;
    meta->first_block = first;
}

allocator_sorted_list::~allocator_sorted_list()
{
    if (_trusted_memory == nullptr)
    {
        return;
    }
    auto* meta = reinterpret_cast<allocator_metadata*>(_trusted_memory);
    meta->mutex->~mutex();
    meta->parent->deallocate(_trusted_memory, meta->size, alignof(std::max_align_t));
}

allocator_sorted_list::allocator_sorted_list(allocator_sorted_list const& other) : _trusted_memory(nullptr)
{
    if (other._trusted_memory == nullptr)
    {
        return;
    }
    auto* other_meta = reinterpret_cast<allocator_metadata*>(other._trusted_memory);
    std::lock_guard other_lock(*other_meta->mutex);
    _trusted_memory = other_meta->parent->allocate(other_meta->size, alignof(std::max_align_t));
    auto* new_meta = reinterpret_cast<allocator_metadata*>(_trusted_memory);
    new_meta->parent = other_meta->parent;
    new_meta->mode = other_meta->mode;
    new_meta->size = other_meta->size;
    new_meta->mutex = new(static_cast<char*>(_trusted_memory) + sizeof(allocator_metadata))std::mutex;
    auto* old_begin = static_cast<char*>(other._trusted_memory);
    auto* new_begin = static_cast<char*>(_trusted_memory);
    std::memcpy(new_begin + allocator_metadata_size, old_begin + allocator_metadata_size, other_meta->size - allocator_metadata_size);
    auto* old_current = reinterpret_cast<block_metadata*>(old_begin + allocator_metadata_size);
    auto* new_current = reinterpret_cast<block_metadata*>(new_begin + allocator_metadata_size);
    while (old_current != nullptr)
    {
        if (old_current->next == nullptr)
        {
            new_current->next = nullptr;
        }
        else
        {
            const auto* old_next = reinterpret_cast<const char*>(old_current->next);
            const ptrdiff_t offset = old_next - old_begin;
            new_current->next = new_begin + offset;
        }
        if (old_current->next == nullptr)
        {
            break;
        }
        old_current = reinterpret_cast<block_metadata*>(old_current->next);
        new_current = reinterpret_cast<block_metadata*>(new_begin + (reinterpret_cast<char*>(old_current) - old_begin));
    }
    new_meta->first_block = new_begin + (reinterpret_cast<char*>(other_meta->first_block) - old_begin);
}

allocator_sorted_list& allocator_sorted_list::operator=(allocator_sorted_list const& other)
{
    if (this == &other)
    {
        return *this;
    }
    allocator_sorted_list copy(other);
    if (_trusted_memory != nullptr)
    {
        auto* meta = reinterpret_cast<allocator_metadata*>(_trusted_memory);
        meta->mutex->~mutex();
        meta->parent->deallocate(
            _trusted_memory,
            meta->size,
            alignof(std::max_align_t));
    }
    _trusted_memory = copy._trusted_memory;
    copy._trusted_memory = nullptr;
    return *this;
}

allocator_sorted_list::allocator_sorted_list(allocator_sorted_list&& other) noexcept
    : _trusted_memory(other._trusted_memory)
{
    other._trusted_memory = nullptr;
}

allocator_sorted_list& allocator_sorted_list::operator=(allocator_sorted_list&& other) noexcept
{
    if (this == &other)
    {
        return *this;
    }
    if (_trusted_memory != nullptr)
    {
        auto* meta = reinterpret_cast<allocator_metadata*>(_trusted_memory);
        meta->mutex->~mutex();
        meta->parent->deallocate(
            _trusted_memory,
            meta->size,
            alignof(std::max_align_t));
    }
    _trusted_memory = other._trusted_memory;
    other._trusted_memory = nullptr;
    return *this;
}

void* allocator_sorted_list::do_allocate_sm(size_t size)
{
    if (_trusted_memory == nullptr)
    {
        throw std::bad_alloc();
    }
    if (size == 0)
    {
        size = 1;
    }
    if (size > std::numeric_limits<size_t>::max() - block_metadata_size)
    {
        throw std::bad_alloc();
    }
    auto* meta = reinterpret_cast<allocator_metadata*>(_trusted_memory);
    std::lock_guard lock(*meta->mutex);
    block_metadata* current = reinterpret_cast<block_metadata*>(meta->first_block);
    block_metadata* selected = nullptr;
    while (current != nullptr)
    {
        if (!is_occupied(current) && get_block_size(current) >= size)
        {
            if (selected == nullptr)
            {
                selected = current;
                if (meta->mode == allocator_with_fit_mode::fit_mode::first_fit)
                {
                    break;
                }
            }
            else
            {
                const size_t current_size = get_block_size(current);
                const size_t selected_size = get_block_size(selected);
                if (meta->mode == allocator_with_fit_mode::fit_mode::the_best_fit &&
                    current_size < selected_size)
                {
                    selected = current;
                }
                if (meta->mode == allocator_with_fit_mode::fit_mode::the_worst_fit &&
                    current_size > selected_size)
                {
                    selected = current;
                }
            }
        }
        current = reinterpret_cast<block_metadata*>(current->next);
    }
    if (selected == nullptr)
    {
        throw std::bad_alloc();
    }
    const size_t old_size = get_block_size(selected);
    if (old_size > size + block_metadata_size)
    {
        auto* new_block = reinterpret_cast<block_metadata*>(
            reinterpret_cast<char*>(selected) + block_metadata_size + size);
        new_block->size = old_size - size - block_metadata_size;
        new_block->next = selected->next;
        selected->next = new_block;
        selected->size = size;
    }
    set_occupied(selected);
    return reinterpret_cast<char*>(selected) + block_metadata_size;
}

void allocator_sorted_list::do_deallocate_sm(void* at)
{
    if (at == nullptr)
    {
        return;
    }
    if (_trusted_memory == nullptr)
    {
        throw std::bad_alloc();
    }
    auto* meta = reinterpret_cast<allocator_metadata*>(_trusted_memory);
    std::lock_guard lock(*meta->mutex);
    block_metadata* current = reinterpret_cast<block_metadata*>(
        meta->first_block);
    block_metadata* target = nullptr;

    while (current != nullptr)
    {
        void* user_address = reinterpret_cast<char*>(current) + block_metadata_size;
        if (user_address == at)
        {
            target = current;
            break;
        }
        current = reinterpret_cast<block_metadata*>(current->next);
    }
    if (target == nullptr || !is_occupied(target))
    {
        throw std::bad_alloc();
    }
    set_free(target);
    current = reinterpret_cast<block_metadata*>(meta->first_block);
    while (current != nullptr)
    {
        auto* next = reinterpret_cast<block_metadata*>(current->next);
        if (next != nullptr &&
            !is_occupied(current) &&
            !is_occupied(next))
        {
            current->size =
                get_block_size(current) +
                block_metadata_size +
                get_block_size(next);
            current->next = next->next;
            continue;
        }
        current = next;
    }
}

void allocator_sorted_list::set_fit_mode(allocator_with_fit_mode::fit_mode mode)
{
    if (_trusted_memory == nullptr)
    {
        return;
    }
    auto* meta = reinterpret_cast<allocator_metadata*>(_trusted_memory);
    std::lock_guard lock(*meta->mutex);
    meta->mode = mode;
}

bool allocator_sorted_list::do_is_equal(const std::pmr::memory_resource& other) const noexcept
{
    return dynamic_cast<const allocator_sorted_list*>(&other) != nullptr;
}

std::vector<allocator_test_utils::block_info> allocator_sorted_list::get_blocks_info() const noexcept
{
    if (_trusted_memory == nullptr)
    {
        return {};
    }
    auto* meta = reinterpret_cast<allocator_metadata*>(_trusted_memory);
    std::lock_guard lock(*meta->mutex);
    return get_blocks_info_inner();
}

std::vector<allocator_test_utils::block_info> allocator_sorted_list::get_blocks_info_inner() const
{
    std::vector<block_info> result;

    if (_trusted_memory == nullptr)
    {
        return result;
    }
    auto* meta = reinterpret_cast<allocator_metadata*>(_trusted_memory);
    auto* current = reinterpret_cast<block_metadata*>(meta->first_block);
    while (current != nullptr)
    {
        result.push_back(
            {
                get_block_size(current),
                is_occupied(current)
            });
        current = reinterpret_cast<block_metadata*>(current->next);
    }
    return result;
}

allocator_sorted_list::sorted_iterator::sorted_iterator()
    : _free_ptr(nullptr),
      _current_ptr(nullptr),
      _trusted_memory(nullptr)
{
}

allocator_sorted_list::sorted_iterator::sorted_iterator(void* trusted)
    : _free_ptr(trusted),
      _current_ptr(trusted),
      _trusted_memory(trusted)
{
}

void* allocator_sorted_list::sorted_iterator::operator*() const noexcept
{
    return _current_ptr;
}

size_t allocator_sorted_list::sorted_iterator::size() const noexcept
{
    if (_current_ptr == nullptr)
    {
        return 0;
    }
    auto* block = reinterpret_cast<block_metadata*>(_current_ptr);
    return get_block_size(block);
}

bool allocator_sorted_list::sorted_iterator::occupied() const noexcept
{
    if (_current_ptr == nullptr)
    {
        return false;
    }
    auto* block = reinterpret_cast<block_metadata*>(_current_ptr);
    return is_occupied(block);
}

allocator_sorted_list::sorted_iterator& allocator_sorted_list::sorted_iterator::operator++() & noexcept
{
    if (_current_ptr == nullptr)
    {
        return *this;
    }
    auto* block = reinterpret_cast<block_metadata*>(_current_ptr);
    _current_ptr = block->next;
    return *this;
}

allocator_sorted_list::sorted_iterator allocator_sorted_list::sorted_iterator::operator++(int n)
{
    (void)n;
    auto copy = *this;
    ++(*this);
    return copy;
}

bool allocator_sorted_list::sorted_iterator::operator==(const sorted_iterator& other) const noexcept
{
    return _current_ptr == other._current_ptr;
}

bool allocator_sorted_list::sorted_iterator::operator!=(const sorted_iterator& other) const noexcept
{
    return !(*this == other);
}

allocator_sorted_list::sorted_free_iterator::sorted_free_iterator()
    : _free_ptr(nullptr)
{
}

allocator_sorted_list::sorted_free_iterator::sorted_free_iterator(void* trusted)
    : _free_ptr(trusted)
{
}

void* allocator_sorted_list::sorted_free_iterator::operator*() const noexcept
{
    return _free_ptr;
}

size_t allocator_sorted_list::sorted_free_iterator::size() const noexcept
{
    if (_free_ptr == nullptr)
    {
        return 0;
    }
    auto* block = reinterpret_cast<block_metadata*>(_free_ptr);
    return get_block_size(block);
}

allocator_sorted_list::sorted_free_iterator&allocator_sorted_list::sorted_free_iterator::operator++() & noexcept
{
    if (_free_ptr == nullptr)
    {
        return *this;
    }
    auto* block = reinterpret_cast<block_metadata*>(_free_ptr);
    block = reinterpret_cast<block_metadata*>(block->next);
    while (block != nullptr && is_occupied(block))
    {
        block = reinterpret_cast<block_metadata*>(block->next);
    }
    _free_ptr = block;
    return *this;
}

allocator_sorted_list::sorted_free_iterator allocator_sorted_list::sorted_free_iterator::operator++(int n)
{
    (void)n;
    auto copy = *this;
    ++(*this);
    return copy;
}

bool allocator_sorted_list::sorted_free_iterator::operator==(const sorted_free_iterator& other) const noexcept
{
    return _free_ptr == other._free_ptr;
}

bool allocator_sorted_list::sorted_free_iterator::operator!=(const sorted_free_iterator& other) const noexcept
{
    return !(*this == other);
}

allocator_sorted_list::sorted_iterator allocator_sorted_list::begin() const noexcept
{
    if (_trusted_memory == nullptr)
    {
        return sorted_iterator(nullptr);
    }
    auto* meta = reinterpret_cast<allocator_metadata*>(_trusted_memory);
    return sorted_iterator(meta->first_block);
}

allocator_sorted_list::sorted_iterator allocator_sorted_list::end() const noexcept
{
    return sorted_iterator(nullptr);
}

allocator_sorted_list::sorted_free_iterator allocator_sorted_list::free_begin() const noexcept
{
    if (_trusted_memory == nullptr)
    {
        return sorted_free_iterator(nullptr);
    }
    auto* meta = reinterpret_cast<allocator_metadata*>(_trusted_memory);
    auto* block = reinterpret_cast<block_metadata*>(meta->first_block);
    while (block != nullptr && is_occupied(block))
    {
        block = reinterpret_cast<block_metadata*>(block->next);
    }
    return sorted_free_iterator(block);
}

allocator_sorted_list::sorted_free_iterator allocator_sorted_list::free_end() const noexcept
{
    return sorted_free_iterator(nullptr);
}