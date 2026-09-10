#pragma once

#include "itch_protocol.hpp"

#include <atomic>
#include <cstdint>

namespace md {

// Busy-wait receiver: non-blocking recvfrom in a tight loop.
// Honesty: each empty poll is still a recvfrom syscall. Linux SO_BUSY_POLL
// (if present) is kernel busy-poll; macOS has no equivalent.
class Receiver {
public:
    static constexpr std::size_t kHist = 1 << 14;

    struct Config {
        const char* group = "239.1.2.3";
        uint16_t port = 19001;
        int pin_cpu = -1;
    };

    struct Stats {
        std::atomic<uint64_t> packets{0};
        std::atomic<uint64_t> messages{0};
        std::atomic<uint64_t> bad{0};
        uint64_t rtt_ns[kHist]{};
        std::atomic<uint32_t> n_rtt{0};
        Decoded last{};
    };

    explicit Receiver(const Config& cfg);
    ~Receiver();

    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;

    bool start();
    void stop();          // signal run_loop to exit; does not close the socket
    void close_fd();      // call after the poll thread has joined
    bool ready() const noexcept { return fd_ >= 0; }

    Stats& stats() noexcept { return stats_; }
    const Stats& stats() const noexcept { return stats_; }

    // One non-blocking poll. Returns messages decoded this call (0 if empty).
    int poll_once() noexcept;

    void run_loop(); // until stop()

private:
    Config cfg_;
    Stats stats_;
    std::atomic<bool> run_{false};
    int fd_ = -1;
    uint8_t buf_[kUdpPayload]{};
};

} // namespace md
