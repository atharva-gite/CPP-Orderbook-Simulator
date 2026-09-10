#include "lob/order_pool.hpp"
#include <new>
#include <cassert>
#include <cstdlib>
#include <cstring>

namespace lob {

OrderPool::OrderPool(size_t capacity, AllocMode mode)
    : capacity_(capacity), mode_(mode)
{
    if (mode_ == AllocMode::Heap) {
        // No slab; each allocate() hits the heap.
        return;
    }
    if (capacity_ == 0) return;
    size_t bytes = capacity_ * sizeof(Order);
    buffer_ = static_cast<Order*>(::operator new(bytes, std::align_val_t(64)));

    freelist_.reserve(capacity_);
    for (size_t i = 0; i < capacity_; ++i) {
        Order* o = reinterpret_cast<Order*>(reinterpret_cast<char*>(buffer_) + i * sizeof(Order));
        freelist_.push_back(o);
    }
}

OrderPool::~OrderPool() {
    if (buffer_) {
        ::operator delete(buffer_, std::align_val_t(64));
    }
}

Order* OrderPool::allocate() {
    if (mode_ == AllocMode::Heap) {
        void* mem = ::operator new(sizeof(Order), std::align_val_t(64));
        auto* o = static_cast<Order*>(mem);
        std::memset(o, 0, sizeof(Order));
        return o;
    }
    if (freelist_.empty()) return nullptr;
    Order* o = freelist_.back();
    freelist_.pop_back();
    std::memset(o, 0, sizeof(Order));
    return o;
}

void OrderPool::deallocate(Order* o) {
    assert(o != nullptr);
    if (mode_ == AllocMode::Heap) {
        ::operator delete(o, std::align_val_t(64));
        return;
    }
    freelist_.push_back(o);
}

} // namespace lob
