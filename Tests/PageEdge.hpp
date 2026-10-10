#pragma once
#include <sys/mman.h>
#include <unistd.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>

// Puts the bytes at the very end of a mapped page that is followed by an inaccessible one, so that a
// parser that reads past what it was given faults in every build. (An exact-size heap buffer shows the
// same read only under AddressSanitizer, which the CI build does not use, and a `std::vector`'s spare
// capacity would hide it altogether.)
inline const char* atEdgeOfPage(const char* data, std::size_t size) {
    static const std::size_t page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    static char* const base = [] {
        void* p = mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED || mprotect(static_cast<char*>(p) + page, page, PROT_NONE) != 0) std::abort();
        return static_cast<char*>(p);
    }();
    if (size > page) std::abort();
    char* const start = base + page - size;
    if (size != 0) std::memcpy(start, data, size);
    return start;
}
