#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

#include "align.hxx"

namespace sonic {

template <std::size_t RingSize, std::size_t EntrySize,
          std::size_t CacheLineSize = kDefaultCacheLineSize>
struct CacheRemap {
  static_assert(RingSize > 0);
  static_assert((RingSize & (RingSize - 1)) == 0,
                "RingSize must be power of two");
  static_assert(EntrySize > 0);
  static_assert(CacheLineSize > 0);
  static_assert((CacheLineSize & (CacheLineSize - 1)) == 0,
                "CacheLineSize must be power of two");

  static constexpr std::size_t kRawEntriesPerLine = CacheLineSize / EntrySize;

  static constexpr std::size_t kEntriesPerLine =
      kRawEntriesPerLine == 0 ? 1 : kRawEntriesPerLine;

  static constexpr bool kCanRemap =
      kEntriesPerLine > 1 && RingSize >= kEntriesPerLine;

  static_assert(!kCanRemap || ((RingSize % kEntriesPerLine) == 0),
                "RingSize must be divisible by entries per cache line");

  static constexpr std::size_t kLineCount =
      kCanRemap ? (RingSize / kEntriesPerLine) : RingSize;

  [[nodiscard]] static constexpr std::size_t Map(std::size_t pos) noexcept {
    pos &= RingSize - 1;

    if constexpr (!kCanRemap) {
      return pos;
    } else {
      const std::size_t offset_in_line = pos % kEntriesPerLine;
      const std::size_t line_index = pos / kEntriesPerLine;
      return offset_in_line * kLineCount + line_index;
    }
  }
};

template <typename T, std::size_t N>
class SCQ {
  static_assert(N > 0, "N must be non-zero");
  static_assert((N & (N - 1)) == 0, "N must be power of two");
  static_assert(
      N < static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()),
      "N must fit in uint32_t indices");
  static_assert(N <= static_cast<std::size_t>(
                         (std::numeric_limits<std::int64_t>::max() - 1) / 3),
                "N is too large for threshold");
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                "SCQ requires lock-free uint64_t atomics");

  static constexpr std::uint64_t kRingSize = 2 * N;
  static constexpr std::uint64_t kRingMask = kRingSize - 1;

 public:
  SCQ() noexcept(std::is_nothrow_default_constructible_v<T>)
      : fq_(/*full=*/true), aq_(/*full=*/false), data_{} {}

  SCQ(const SCQ&) = delete;
  SCQ& operator=(const SCQ&) = delete;

  [[nodiscard]] bool Enqueue(const T& value) noexcept(
      std::is_nothrow_copy_assignable_v<T>)
    requires(std::is_nothrow_copy_assignable_v<T>)
  {
    auto index = fq_.DequeueRelaxed();
    if (!index) return false;

    // Synchronizes with the previous consumer's release fence before
    // fq_.EnqueueRelaxed(index). This prevents racing with its data_[index]
    // read.
    std::atomic_thread_fence(std::memory_order_acquire);

    data_[*index] = value;

    // Publish payload before making index visible in aq_.
    std::atomic_thread_fence(std::memory_order_release);
    aq_.EnqueueRelaxed(*index);

    return true;
  }

  [[nodiscard]] bool Dequeue(T& out) noexcept(
      std::is_nothrow_copy_assignable_v<T>)
    requires(std::is_nothrow_copy_assignable_v<T>)
  {
    auto index = aq_.DequeueRelaxed();
    if (!index) return false;

    // Synchronizes with producer's release fence before
    // aq_.EnqueueRelaxed(index).
    std::atomic_thread_fence(std::memory_order_acquire);

    out = data_[*index];

    // Finish reading/copying payload before returning index to fq_.
    std::atomic_thread_fence(std::memory_order_release);
    fq_.EnqueueRelaxed(*index);

    return true;
  }

  [[nodiscard]] bool TryEnqueue(const T& value) noexcept(
      std::is_nothrow_copy_assignable_v<T>)
    requires(std::is_nothrow_copy_assignable_v<T>)
  {
    return Enqueue(value);
  }

  [[nodiscard]] bool TryDequeue(T& out) noexcept(
      std::is_nothrow_copy_assignable_v<T>)
    requires(std::is_nothrow_copy_assignable_v<T>)
  {
    return Dequeue(out);
  }

 private:
  class IndexSCQ {
    static constexpr std::uint32_t kCapacity = static_cast<std::uint32_t>(N);

    // Entry layout:
    //
    //   bit 63        : IsSafe
    //   bits 62..32  : 31-bit cycle
    //   bits 31..0   : index
    //
    // kIndexNil is all ones, so dequeue can consume with fetch_or(kIndexMask).
    static constexpr std::uint32_t kIndexNil =
        std::numeric_limits<std::uint32_t>::max();

    static constexpr std::uint32_t kCycleBits = 31;
    static constexpr std::uint32_t kCycleMask =
        (std::uint32_t{1} << kCycleBits) - 1;
    static constexpr std::uint32_t kCycleSign = std::uint32_t{1}
                                                << (kCycleBits - 1);

    static constexpr std::uint64_t kIndexMask = 0xffffffffull;
    static constexpr std::uint64_t kSafeBit = 1ull << 63;
    static constexpr std::uint64_t kCycleShift = 32;

    static constexpr std::int64_t kThreshold =
        3 * static_cast<std::int64_t>(N) - 1;

    using Remap = CacheRemap<kRingSize, sizeof(std::atomic<std::uint64_t>)>;

   public:
    explicit IndexSCQ(bool full) noexcept
        : head_(full ? (kRingSize - N) : kRingSize),
          tail_(kRingSize),
          threshold_(full ? kThreshold : -1) {
      for (auto& entry : entries_) {
        entry.store(MakeEntry(0, true, kIndexNil), std::memory_order_relaxed);
      }

      // Full queue initially contains exactly N usable indices.
      //
      // Tickets [N, 2N) are initialized with indices [0, N).
      // Once drained, head == tail == 2N, matching the empty state.
      if (full) {
        for (std::uint32_t i = 0; i < kCapacity; ++i) {
          const std::uint64_t ticket = (kRingSize - N) + i;
          entries_[Slot(ticket)].store(MakeEntry(CycleOf(ticket), true, i),
                                       std::memory_order_relaxed);
        }
      }
    }

    void EnqueueRelaxed(std::uint32_t index) noexcept {
      for (;;) {
        const std::uint64_t tail =
            tail_.fetch_add(1, std::memory_order_relaxed);

        const std::uint32_t tail_cycle = CycleOf(tail);
        const std::uint64_t slot = Slot(tail);

      reload:
        std::uint64_t old = entries_[slot].load(std::memory_order_acquire);
        const std::uint32_t old_cycle = Cycle(old);

        if (CycleLess(old_cycle, tail_cycle) && Index(old) == kIndexNil &&
            (IsSafe(old) || head_.load(std::memory_order_relaxed) <= tail)) {
          const std::uint64_t desired = MakeEntry(tail_cycle, true, index);

          if (!entries_[slot].compare_exchange_weak(
                  old, desired, std::memory_order_relaxed,
                  std::memory_order_relaxed)) {
            goto reload;
          }

          if (threshold_.load(std::memory_order_relaxed) != kThreshold) {
            threshold_.store(kThreshold, std::memory_order_relaxed);
          }

          return;
        }
      }
    }

    [[nodiscard]] std::optional<std::uint32_t> DequeueRelaxed() noexcept {
      if (threshold_.load(std::memory_order_relaxed) < 0) {
        return std::nullopt;
      }

      for (;;) {
        const std::uint64_t head =
            head_.fetch_add(1, std::memory_order_relaxed);

        const std::uint32_t head_cycle = CycleOf(head);
        const std::uint64_t slot = Slot(head);

      reload:
        std::uint64_t old = entries_[slot].load(std::memory_order_relaxed);
        const std::uint32_t old_cycle = Cycle(old);
        const std::uint32_t old_index = Index(old);

        if (old_cycle == head_cycle) {
          const std::uint64_t prev =
              entries_[slot].fetch_or(kIndexMask, std::memory_order_relaxed);

          const std::uint32_t index = Index(prev);
          if (index == kIndexNil) return std::nullopt;

          return index;
        }

        std::uint64_t desired = MakeEntry(old_cycle, false, old_index);

        if (old_index == kIndexNil) {
          desired = MakeEntry(head_cycle, IsSafe(old), kIndexNil);
        }

        if (CycleLess(old_cycle, head_cycle)) {
          if (!entries_[slot].compare_exchange_weak(
                  old, desired, std::memory_order_release,
                  std::memory_order_relaxed)) {
            goto reload;
          }
        }

        const std::uint64_t tail = tail_.load(std::memory_order_relaxed);

        if (tail <= head + 1) {
          Catchup(tail, head + 1);
          threshold_.fetch_sub(1, std::memory_order_relaxed);
          return std::nullopt;
        }

        if (threshold_.fetch_sub(1, std::memory_order_relaxed) <= 0) {
          return std::nullopt;
        }
      }
    }

   private:
    static constexpr std::uint64_t Slot(std::uint64_t ticket) noexcept {
      return static_cast<std::uint64_t>(
          Remap::Map(static_cast<std::size_t>(ticket & kRingMask)));
    }

    static constexpr std::uint32_t CycleOf(std::uint64_t ticket) noexcept {
      return static_cast<std::uint32_t>((ticket / kRingSize) & kCycleMask);
    }

    static constexpr std::uint64_t MakeEntry(std::uint32_t cycle, bool safe,
                                             std::uint32_t index) noexcept {
      return (safe ? kSafeBit : 0) |
             (static_cast<std::uint64_t>(cycle & kCycleMask) << kCycleShift) |
             static_cast<std::uint64_t>(index);
    }

    static constexpr std::uint32_t Cycle(std::uint64_t entry) noexcept {
      return static_cast<std::uint32_t>((entry >> kCycleShift) & kCycleMask);
    }

    static constexpr std::uint32_t Index(std::uint64_t entry) noexcept {
      return static_cast<std::uint32_t>(entry & kIndexMask);
    }

    static constexpr bool IsSafe(std::uint64_t entry) noexcept {
      return (entry & kSafeBit) != 0;
    }

    static constexpr bool CycleLess(std::uint32_t a, std::uint32_t b) noexcept {
      // 31-bit modular less-than.
      return ((a - b) & kCycleSign) != 0;
    }

    void Catchup(std::uint64_t tail, std::uint64_t head) noexcept {
      while (tail < head &&
             !tail_.compare_exchange_weak(tail, head, std::memory_order_relaxed,
                                          std::memory_order_relaxed)) {
        head = head_.load(std::memory_order_relaxed);
        tail = tail_.load(std::memory_order_relaxed);
        if (tail >= head) break;
      }
    }

    SONIC_CACHE_ALIGN std::atomic<std::uint64_t> head_;
    SONIC_CACHE_ALIGN std::atomic<std::uint64_t> tail_;
    SONIC_CACHE_ALIGN std::atomic<std::int64_t> threshold_;
    SONIC_CACHE_ALIGN
    std::array<std::atomic<std::uint64_t>, kRingSize> entries_;
  };

  IndexSCQ fq_;
  IndexSCQ aq_;

  SONIC_CACHE_ALIGN std::array<T, N> data_;
};

}  // namespace sonic