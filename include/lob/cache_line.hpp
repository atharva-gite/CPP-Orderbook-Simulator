#pragma once

#include <cstddef>

namespace lob {

// Apple libc++ often omits std::hardware_destructive_interference_size
// (ABI-sensitive). 64 bytes matches the Order alignas and this SoC.
inline constexpr std::size_t kCacheLine = 64;

} // namespace lob
