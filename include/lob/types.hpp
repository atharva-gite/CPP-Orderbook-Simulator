// Fundamental types for lob
#pragma once

#include <cstdint>

namespace lob {

// Side of an order: Buy or Sell
enum class Side : uint8_t {
    Buy = 0,
    Sell = 1
};

// Order execution type
enum class OrderType : uint8_t {
    Limit = 0,
    Market = 1,
    IOC = 2,
    FOK = 3
};

// Price type: use fixed-point integer ticks (int64_t) rather than floating
// point. Floating point values (float/double) are inappropriate for prices
// because they are binary fractions which cannot exactly represent many
// decimal fractions, leading to rounding errors and non-deterministic
// comparisons. Using integer ticks ensures exact arithmetic and ordering.
using price_t = int64_t; // ticks

// Quantity type
using qty_t = uint64_t;

// Order identifier
using order_id_t = uint64_t;

} // namespace lob
