#pragma once

#include "lob/cache_line.hpp"

#include <atomic>
#include <cstddef>
#include <type_traits>

namespace lob {

// Single-producer / single-consumer bounded ring.
// Wait-free try_push / try_pop when not full / not empty. No CAS.
// Matching thread -> MD publisher is this shape (one writer).
template <typename T, std::size_t Capacity>
class SpscRing {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>);

    static constexpr std::size_t kMask = Capacity - 1;

public:
    bool try_push(const T& v) noexcept {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        const std::size_t n = h + 1;
        // acquire: see consumer's tail release so we don't overwrite unread slots
        if (n - tail_.load(std::memory_order_acquire) > Capacity) return false;
        buf_[h & kMask] = v;
        // release: publishes buf_[h] to the consumer
        head_.store(n, std::memory_order_release);
        return true;
    }

    bool try_pop(T& out) noexcept {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        // acquire: sees producer's head release and therefore buf_[t]
        if (t == head_.load(std::memory_order_acquire)) return false;
        out = buf_[t & kMask];
        // release: slot t is free for the producer
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    T buf_[Capacity]{};
    alignas(kCacheLine) std::atomic<std::size_t> head_{0};
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
};

} // namespace lob
