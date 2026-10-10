#pragma once
#include <cstdint>

// Changes with anything that can alter which orders are accepted or which of several equally good
// allocations comes back: the pivot rule, the chunk and block sizes, the starting tree, the limits, the
// catalog and the schedule. A recorded batch says which version produced it.
inline constexpr std::uint32_t SOLVER_VERSION = 1;
