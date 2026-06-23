#pragma once

#include <atomic>
#include <cstdint>

namespace sonic::mem {

struct EpochParticipant {
  std::atomic<uintptr_t> next;
  std::atomic<uint64_t> local_epoch;
};

class EpochDomain {
  static constexpr uint64_t kInactiveEpoch =
      std::numeric_limits<uint64_t>::max();

 public:
  EpochDomain() noexcept : epoch_{0} {}

  EpochParticipant* Register();

  void Pin() noexcept;

  void Unpin() noexcept;

  void Deregister(EpochParticipant*) noexcept;

  void Retire(void* ptr, void (*fn_delete)(void*));

 private:
  void PushFront(EpochParticipant* n) noexcept;

  bool Remove(EpochParticipant* target) noexcept;

  std::atomic<uint64_t> epoch_;

  std::atomic<uintptr_t> participants_;
};

}  // namespace sonic::mem