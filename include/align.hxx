#pragma once

#include <new>

#if defined(__cpp_lib_hardware_interference_size)
inline constexpr std::size_t kDefaultCacheLineSize =
    std::hardware_destructive_interference_size;
#else
inline constexpr std::size_t kDefaultCacheLineSize = 64;
#endif

#define SONIC_CACHE_ALIGN alignas(kDefaultCacheLineSize)