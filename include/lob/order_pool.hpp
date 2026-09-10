#pragma once

#include "lob/order.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace lob {

// Pool: pre-sized 64-byte-aligned slab + freelist (Phase 1).
// Heap: per-order aligned new/delete — used as the A/B toggle in benches.
enum class AllocMode : uint8_t { Pool = 0, Heap = 1 };

class OrderPool {
public:
    explicit OrderPool(size_t capacity, AllocMode mode = AllocMode::Pool);
    ~OrderPool();

    Order* allocate();
    void deallocate(Order* o);

    size_t capacity() const noexcept { return capacity_; }
    size_t free_count() const noexcept { return freelist_.size(); }
    AllocMode mode() const noexcept { return mode_; }

    // Non-copyable
    OrderPool(const OrderPool&) = delete;
    OrderPool& operator=(const OrderPool&) = delete;

private:
    Order* buffer_ = nullptr; // contiguous block (Pool mode only)
    size_t capacity_ = 0;
    AllocMode mode_ = AllocMode::Pool;
    std::vector<Order*> freelist_; // stack of free pointers (pre-reserved)
};

} // namespace lob
