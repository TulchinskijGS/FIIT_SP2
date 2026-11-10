#include "../include/allocator_global_heap.h"

#include <new>

std::mutex allocator_global_heap::mutex_;

allocator_global_heap::allocator_global_heap()
{
}

allocator_global_heap::~allocator_global_heap()
{
}

void *allocator_global_heap::do_allocate_sm(size_t size)
{
    if (size == 0)
    {
        size = 1;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    return ::operator new(size);
}

void allocator_global_heap::do_deallocate_sm(void *at)
{
    if (at == nullptr)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    ::operator delete(at);
}

allocator_global_heap::allocator_global_heap(
    allocator_global_heap const &other)
{
}

allocator_global_heap &allocator_global_heap::operator=(
    allocator_global_heap const &other)
{
    return *this;
}

allocator_global_heap::allocator_global_heap(
    allocator_global_heap &&other) noexcept
{
}

allocator_global_heap &allocator_global_heap::operator=(
    allocator_global_heap &&other) noexcept
{
    return *this;
}

bool allocator_global_heap::do_is_equal(
    const std::pmr::memory_resource &other) const noexcept
{
    return dynamic_cast<const allocator_global_heap*>(&other) != nullptr;
}