#pragma once

#include "itch_protocol.hpp"
#include "lob/order_book.hpp"
#include "lob/spsc_queue.hpp"

#include <atomic>
#include <cstdint>
#include <thread>

namespace md {

// UDP multicast publisher. Matching thread calls try_publish_* (SPSC enqueue).
// A dedicated thread batches into MTU-sized datagrams and sendto()s.
//
// Hot path (try_publish_* + publisher loop pack/send) does not allocate:
// pre-sized SPSC, stack datagram buffer, pre-sized latency rings.
class Publisher {
public:
    static constexpr std::size_t kQueue = 1 << 16;
    static constexpr std::size_t kHist = 1 << 14;

    struct Config {
        const char* group = "239.1.2.3";
        uint16_t port = 19001;
        int pin_cpu = -1;          // publisher thread; matching should use another
        int ttl = 1;
        bool loopback = true;      // required for same-host tests
    };

    struct Stats {
        std::atomic<uint64_t> enqueued{0};
        std::atomic<uint64_t> dropped{0}; // SPSC full
        std::atomic<uint64_t> packets{0};
        std::atomic<uint64_t> messages{0};
        std::atomic<uint64_t> send_fail{0};
        // Filled by publisher thread; read after join/stop for percentiles.
        uint64_t pub_ns[kHist]{};
        uint64_t ser_ns[kHist]{};
        std::atomic<uint32_t> n_pub{0};
        std::atomic<uint32_t> n_ser{0};
    };

    explicit Publisher(const Config& cfg);
    ~Publisher();

    Publisher(const Publisher&) = delete;
    Publisher& operator=(const Publisher&) = delete;

    bool start();
    void stop();

    // Matching-thread API. No heap. false = queue full (drop).
    bool try_publish_add(lob::order_id_t id, lob::Side side, lob::price_t px, lob::qty_t qty,
                         uint64_t event_ns) noexcept;
    bool try_publish_cancel(lob::order_id_t id, uint64_t event_ns) noexcept;
    bool try_publish_execute(lob::order_id_t id, lob::price_t px, lob::qty_t qty,
                             uint64_t event_ns) noexcept;
    bool try_publish_snapshot(const lob::OrderBook& book, uint64_t event_ns) noexcept;

    Stats& stats() noexcept { return stats_; }
    const Stats& stats() const noexcept { return stats_; }

    // True after the UDP socket is bound/connected for send.
    bool ready() const noexcept { return fd_ >= 0; }

private:
    void thread_main();
    bool enqueue(const Event& e) noexcept;
    void record(uint64_t* buf, std::atomic<uint32_t>& n, uint64_t v) noexcept;

    Config cfg_;
    lob::SpscRing<Event, kQueue> q_;
    Stats stats_;
    std::thread th_;
    std::atomic<bool> run_{false};
    int fd_ = -1;
    uint64_t pkt_seq_ = 0;
};

} // namespace md
