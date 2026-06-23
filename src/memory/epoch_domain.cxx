#include "epoch_domain.hxx"

#include <new>

#include "../include/utils.hxx"

namespace sonic::mem {

static thread_local EpochParticipant* tl_rec;
static thread_local uint64_t tl_entry_gurad;

void EpochDomain::PushFront(EpochParticipant* n) noexcept {
  uintptr_t old_head = participants_.load(std::memory_order_relaxed);
  for (;;) {
    (n->next).store(
        reinterpret_cast<uintptr_t>(Unmarked<EpochParticipant>(old_head)),
        std::memory_order_relaxed);
    if (participants_.compare_exchange_weak(
            old_head, reinterpret_cast<uintptr_t>(n), std::memory_order_release,
            std::memory_order_relaxed)) {
      break;
    }
  }
}

bool EpochDomain::Remove(EpochParticipant* target) noexcept {
retry:
  std::atomic<uintptr_t>* prev_next = &participants_;
  uintptr_t curr_raw = prev_next->load(std::memory_order_acquire);
  EpochParticipant* curr = Unmarked<EpochParticipant>(curr_raw);

  while (curr != nullptr) {
    uintptr_t next_raw = (curr->next).load(std::memory_order_relaxed);

    if (IsMarked(next_raw)) {
      uintptr_t expected = curr_raw;
      if (prev_next->compare_exchange_weak(expected, Unmarked(next_raw),
                                           std::memory_order_release,
                                           std::memory_order_relaxed)) {
        curr_raw = prev_next->load(std::memory_order_acquire);
        curr = Unmarked<EpochParticipant>(curr_raw);
        continue;
      }
      goto retry;
    }

    if (curr == target) {
      uintptr_t expected = next_raw;
      if ((curr->next)
              .compare_exchange_weak(expected, Marked(next_raw),
                                     std::memory_order_release,
                                     std::memory_order_relaxed)) {
        uintptr_t new_prev_next =
            IsMarked(curr_raw) ? Marked(next_raw) : next_raw;
        prev_next->compare_exchange_weak(curr_raw, new_prev_next,
                                         std::memory_order_release,
                                         std::memory_order_relaxed);
        return true;
      }
      if (IsMarked(expected)) return false;
      goto retry;
    }

    prev_next = &(curr->next);
    curr_raw = (curr->next).load(std::memory_order_acquire);
    curr = Unmarked<EpochParticipant>(curr_raw);
  }

  return false;  // reached end of list — target not present
}

EpochParticipant* EpochDomain::Register() {
  EpochParticipant* thrd_rec = new (std::nothrow) EpochParticipant();
  if (thrd_rec == nullptr) return nullptr;

  PushFront(thrd_rec);

  return thrd_rec;
}

void EpochDomain::Pin() noexcept {
  if (tl_entry_gurad++ > 0) return;

  // read global epoch
  uint64_t local_epoch = epoch_.load(std::memory_order_relaxed);
  // publish observed epoch
  tl_rec->local_epoch.store(local_epoch, std::memory_order_relaxed);
}

void EpochDomain::Retire(void* ptr, void (*fn_delete)(void*)) {}

void EpochDomain::Unpin() noexcept {
  if (tl_entry_gurad-- > 0) return;

  tl_rec->local_epoch.store(kInactiveEpoch, std::memory_order_relaxed);
}

void EpochDomain::Deregister(EpochParticipant* thrd_rec) noexcept {
  Remove(thrd_rec);
}

}  // namespace sonic::mem