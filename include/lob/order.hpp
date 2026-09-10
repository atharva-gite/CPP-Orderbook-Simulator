// Order layout for lob
#pragma once

#include "lob/types.hpp"
#include <cstddef>
#include <cstdint>

namespace lob {


struct alignas(64) Order {
    // Bookkeeping / identity
    order_id_t order_id;   // 8
    qty_t qty;             // total quantity (visible+hidden) 8
    uint64_t timestamp;    // 8  (steady_clock epoch in ns)
    Order* prev;           // 8

    // Hot matching fields grouped together
    price_t price;         // 8
    qty_t remaining_qty;   // visible remaining qty at this moment 8
    Order* next;           // 8

    // Iceberg & stop support
    qty_t display_qty;     // configured display slice for iceberg (0 => not iceberg) 8
    qty_t hidden_qty;      // remaining hidden quantity not currently displayed 8

    // Small fields
    uint8_t side;          // 1
    uint8_t type;          // 1
    uint8_t stop_type;     // 0 = none, 1 = stop-market, 2 = stop-limit
    uint8_t flags;         // padding/flags
    price_t stop_price;    // 8 (stop trigger price for stop orders)

    // Total: may exceed single cache line depending on platform; static_assert
};

static_assert(alignof(Order) == 64, "Order must be 64-byte aligned");
static_assert(sizeof(Order) % 64 == 0, "Order size should be multiple of cache line (64 bytes)");

// Layout comment (byte offsets):
// 0x00 order_id (8)
// 0x08 qty (8)
// 0x10 timestamp (8)
// 0x18 prev (8)
// 0x20 price (8)
// 0x28 remaining_qty (8)
// 0x30 next (8)
// 0x38 side (1)
// 0x39 type (1)
// 0x3A flags (2)
// 0x3C reserved (4)

} // namespace lob
