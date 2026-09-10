#pragma once

#include "lob/cache_line.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

namespace lob {

// Bounded MPSC ring (power-of-two capacity).
//
// What is actually lock-free:
//   Producer try_push uses a CAS on enqueue_pos_ to claim a unique slot.
//   That is lock-free in the progress sense (the system as a whole advances;
//   no OS mutex, no blocking syscall). It is NOT wait-free: a producer can
//   starve under CAS contention, and a FULL queue makes try_push fail so
//   callers that spin on full are spinning — that spin is not lock-free
//   progress for that thread.
//
//   The single consumer try_pop never CASes the write index; it is wait-free
//   given a non-empty queue.
//
// What this is not:
//   A mutex or ticket spinlock. Do not describe the CAS loop as "wait-free"
//   or "uncontended"; many producers still serialize on enqueue_pos_.
//
// Memory orders (Vyukov-style slot sequences):
//   Data is published through slot.seq, not through the position atomics.
template <typename T, std::size_t Capacity>
class MpscRing {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>,
                  "T is copied into slots without constructor calls");

    static constexpr std::size_t kMask = Capacity - 1;

    struct Slot {
        std::atomic<std::size_t> seq;
        T data;
        Slot() : seq(0), data{} {}
    };

public:
    MpscRing() {
        for (std::size_t i = 0; i < Capacity; ++i) {
            slots_[i].seq.store(i, std::memory_order_relaxed);
        }
        enqueue_pos_.store(0, std::memory_order_relaxed);
        dequeue_pos_.store(0, std::memory_order_relaxed);
    }

    MpscRing(const MpscRing&) = delete;
    MpscRing& operator=(const MpscRing&) = delete;

    // Returns false if the ring is full. Does not block.
    bool try_push(const T& value) noexcept {
        std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            Slot& slot = slots_[pos & kMask];
            // acquire: if seq==pos the consumer has released this slot as empty
            // (or it is the initial seq=i), so we may write data.
            const std::size_t seq = slot.seq.load(std::memory_order_acquire);
            const std::intptr_t dif =
                static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);
            if (dif == 0) {
                // relaxed CAS: we only need exclusive claim of `pos`.
                // The later seq store(release) publishes `data`.
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1,
                                                       std::memory_order_relaxed,
                                                       std::memory_order_relaxed)) {
                    slot.data = value;
                    // release: consumer's acquire on seq will see `data`.
                    slot.seq.store(pos + 1, std::memory_order_release);
                    return true;
                }
            } else if (dif < 0) {
                return false; // full
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
    }

    // Single consumer. Returns false if empty.
    bool try_pop(T& out) noexcept {
        const std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        Slot& slot = slots_[pos & kMask];
        // acquire: pairs with producer's seq store(release); sees `data`.
        const std::size_t seq = slot.seq.load(std::memory_order_acquire);
        const std::intptr_t dif = static_cast<std::intptr_t>(seq) -
                                  static_cast<std::intptr_t>(pos + 1);
        if (dif < 0) return false;
        out = slot.data;
        // release: next producer wrap-around acquire sees the slot as free
        // (seq == pos+Capacity, which equals the next-lap pos).
        slot.seq.store(pos + Capacity, std::memory_order_release);
        // relaxed: only this thread stores dequeue_pos_.
        dequeue_pos_.store(pos + 1, std::memory_order_relaxed);
        return true;
    }

    bool empty() const noexcept {
        const std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        const std::size_t seq = slots_[pos & kMask].seq.load(std::memory_order_acquire);
        return static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1) < 0;
    }

    static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    alignas(kCacheLine) Slot slots_[Capacity];

    // Producer-claimed write index. Separate line from dequeue_pos_ so the
    // consumer's stores do not bounce the producer's cache line.
    alignas(kCacheLine) std::atomic<std::size_t> enqueue_pos_{0};
    alignas(kCacheLine) std::atomic<std::size_t> dequeue_pos_{0};
};

} // namespace lob
