#ifndef MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_H
#define MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_H

#include <cstddef>
#include <utility>
#include <string>

class allocator_dbg_helper
{

public:

    using block_size_t = size_t;
    using block_pointer_t = void*;

    virtual ~allocator_dbg_helper() noexcept = default;

protected:

    static std::string get_dump(char* data, size_t size);

    static std::string dump_byte(char byte);

    static char int_to_char(int val);
};

#endif //MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_H