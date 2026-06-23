#pragma once

#include <cstdint>

namespace sonic {

constexpr uintptr_t kUnmarkMask = ~0x1;

template <typename T>
T* Unmarked(uintptr_t rawptr) noexcept {
  return reinterpret_cast<T*>(rawptr & kUnmarkMask);
}

uintptr_t Unmarked(uintptr_t rawptr) noexcept {
  return rawptr & kUnmarkMask;
}
uintptr_t Marked(uintptr_t ptr) noexcept {
  return ptr | ~kUnmarkMask;
}
bool IsMarked(uintptr_t rawptr) noexcept {
  return (rawptr & ~kUnmarkMask) == 1;
}
}  // namespace sonic