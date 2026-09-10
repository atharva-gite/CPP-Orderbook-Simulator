#pragma once

#include "lob/types.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>

namespace md {

// ITCH-style binary market data. Packed, big-endian on the wire.
// No implicit padding (#pragma pack). Sizes are static_asserted.

inline constexpr uint32_t kMagic = 0x49544348u; // 'ITCH'
inline constexpr uint8_t kVersion = 1;
inline constexpr std::size_t kUdpPayload = 1400; // stay under typical Ethernet MTU

enum class MsgType : uint8_t {
    Add = 1,
    Cancel = 2,
    Execute = 3,
    Snapshot = 4
};

inline uint32_t bswap32(uint32_t x) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap32(x);
#else
    return x;
#endif
}
inline uint64_t bswap64(uint64_t x) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap64(x);
#else
    return x;
#endif
}
inline uint16_t bswap16(uint16_t x) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return static_cast<uint16_t>((x << 8) | (x >> 8));
#else
    return x;
#endif
}
inline int64_t bswap_i64(int64_t x) {
    return static_cast<int64_t>(bswap64(static_cast<uint64_t>(x)));
}

#pragma pack(push, 1)
struct PktHdr {
    uint32_t magic_be;
    uint8_t version;
    uint8_t count;
    uint16_t nbytes_be; // payload bytes after this header
    uint64_t pkt_seq_be;
    uint64_t send_ns_be;
};

struct AddBody {
    uint64_t order_id_be;
    uint8_t side; // 0 buy, 1 sell
    int64_t price_be;
    uint64_t qty_be;
    uint64_t event_ns_be;
};

struct CancelBody {
    uint64_t order_id_be;
    uint64_t event_ns_be;
};

struct ExecuteBody {
    uint64_t order_id_be;
    int64_t price_be;
    uint64_t qty_be;
    uint64_t event_ns_be;
};

// Top-of-book only. Full depth is not a single fixed-size struct.
struct SnapshotBody {
    uint8_t flags; // bit0 has_bid, bit1 has_ask
    int64_t bid_be;
    uint64_t bid_qty_be;
    int64_t ask_be;
    uint64_t ask_qty_be;
    uint64_t event_ns_be;
};
#pragma pack(pop)

static_assert(sizeof(PktHdr) == 24, "PktHdr packed");
static_assert(sizeof(AddBody) == 33, "AddBody packed");
static_assert(sizeof(CancelBody) == 16, "CancelBody packed");
static_assert(sizeof(ExecuteBody) == 32, "ExecuteBody packed");
static_assert(sizeof(SnapshotBody) == 41, "SnapshotBody packed");

inline constexpr std::size_t kMaxMsgBytes = 1 + sizeof(SnapshotBody);

inline uint64_t now_ns() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// Host-side event (matching thread -> publisher). Not on the wire.
struct Event {
    MsgType type = MsgType::Add;
    lob::Side side = lob::Side::Buy;
    lob::order_id_t order_id = 0;
    lob::price_t price = 0;
    lob::qty_t qty = 0;
    lob::price_t bid = 0;
    lob::qty_t bid_qty = 0;
    lob::price_t ask = 0;
    lob::qty_t ask_qty = 0;
    uint8_t snap_flags = 0;
    uint64_t event_ns = 0;
};

struct Decoded {
    MsgType type = MsgType::Add;
    lob::Side side = lob::Side::Buy;
    lob::order_id_t order_id = 0;
    lob::price_t price = 0;
    lob::qty_t qty = 0;
    lob::price_t bid = 0;
    lob::qty_t bid_qty = 0;
    lob::price_t ask = 0;
    lob::qty_t ask_qty = 0;
    uint8_t snap_flags = 0;
    uint64_t event_ns = 0;
};

inline std::size_t pack_msg(uint8_t* dst, const Event& e) {
    dst[0] = static_cast<uint8_t>(e.type);
    switch (e.type) {
        case MsgType::Add: {
            AddBody b{};
            b.order_id_be = bswap64(e.order_id);
            b.side = static_cast<uint8_t>(e.side);
            b.price_be = bswap_i64(e.price);
            b.qty_be = bswap64(e.qty);
            b.event_ns_be = bswap64(e.event_ns);
            std::memcpy(dst + 1, &b, sizeof(b));
            return 1 + sizeof(b);
        }
        case MsgType::Cancel: {
            CancelBody b{};
            b.order_id_be = bswap64(e.order_id);
            b.event_ns_be = bswap64(e.event_ns);
            std::memcpy(dst + 1, &b, sizeof(b));
            return 1 + sizeof(b);
        }
        case MsgType::Execute: {
            ExecuteBody b{};
            b.order_id_be = bswap64(e.order_id);
            b.price_be = bswap_i64(e.price);
            b.qty_be = bswap64(e.qty);
            b.event_ns_be = bswap64(e.event_ns);
            std::memcpy(dst + 1, &b, sizeof(b));
            return 1 + sizeof(b);
        }
        case MsgType::Snapshot: {
            SnapshotBody b{};
            b.flags = e.snap_flags;
            b.bid_be = bswap_i64(e.bid);
            b.bid_qty_be = bswap64(e.bid_qty);
            b.ask_be = bswap_i64(e.ask);
            b.ask_qty_be = bswap64(e.ask_qty);
            b.event_ns_be = bswap64(e.event_ns);
            std::memcpy(dst + 1, &b, sizeof(b));
            return 1 + sizeof(b);
        }
    }
    return 0;
}

// Decode one message. Returns bytes consumed, 0 on error.
inline std::size_t unpack_msg(const uint8_t* src, std::size_t avail, Decoded& out) {
    if (avail < 1) return 0;
    out.type = static_cast<MsgType>(src[0]);
    switch (out.type) {
        case MsgType::Add: {
            if (avail < 1 + sizeof(AddBody)) return 0;
            AddBody b{};
            std::memcpy(&b, src + 1, sizeof(b));
            out.order_id = bswap64(b.order_id_be);
            out.side = static_cast<lob::Side>(b.side);
            out.price = bswap_i64(b.price_be);
            out.qty = bswap64(b.qty_be);
            out.event_ns = bswap64(b.event_ns_be);
            return 1 + sizeof(b);
        }
        case MsgType::Cancel: {
            if (avail < 1 + sizeof(CancelBody)) return 0;
            CancelBody b{};
            std::memcpy(&b, src + 1, sizeof(b));
            out.order_id = bswap64(b.order_id_be);
            out.event_ns = bswap64(b.event_ns_be);
            return 1 + sizeof(b);
        }
        case MsgType::Execute: {
            if (avail < 1 + sizeof(ExecuteBody)) return 0;
            ExecuteBody b{};
            std::memcpy(&b, src + 1, sizeof(b));
            out.order_id = bswap64(b.order_id_be);
            out.price = bswap_i64(b.price_be);
            out.qty = bswap64(b.qty_be);
            out.event_ns = bswap64(b.event_ns_be);
            return 1 + sizeof(b);
        }
        case MsgType::Snapshot: {
            if (avail < 1 + sizeof(SnapshotBody)) return 0;
            SnapshotBody b{};
            std::memcpy(&b, src + 1, sizeof(b));
            out.snap_flags = b.flags;
            out.bid = bswap_i64(b.bid_be);
            out.bid_qty = bswap64(b.bid_qty_be);
            out.ask = bswap_i64(b.ask_be);
            out.ask_qty = bswap64(b.ask_qty_be);
            out.event_ns = bswap64(b.event_ns_be);
            return 1 + sizeof(b);
        }
    }
    return 0;
}

} // namespace md
