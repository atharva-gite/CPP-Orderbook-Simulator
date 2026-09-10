#include "md_receiver.hpp"
#include "thread_pin.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace md {

Receiver::Receiver(const Config& cfg) : cfg_(cfg) {}

Receiver::~Receiver() {
    stop();
    close_fd();
}

bool Receiver::start() {
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) return false;

    int yes = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#ifdef SO_REUSEPORT
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#endif
    int rcv = 1 << 20;
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));

#ifdef SO_BUSY_POLL
    // Kernel busy-poll (Linux). Microseconds to spin in the driver before
    // sleeping. Not available on macOS — we still userspace-spin on EAGAIN.
    int bp = 50;
    ::setsockopt(fd_, SOL_SOCKET, SO_BUSY_POLL, &bp, sizeof(bp));
#endif

    int flags = ::fcntl(fd_, F_GETFL, 0);
    ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg_.port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    ip_mreq mreq{};
    if (::inet_pton(AF_INET, cfg_.group, &mreq.imr_multiaddr) != 1) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    mreq.imr_interface.s_addr = htonl(INADDR_LOOPBACK);
    if (::setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        if (::setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }
    }

    int loop = 1;
    ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

    run_.store(true, std::memory_order_release);
    return true;
}

void Receiver::stop() {
    run_.store(false, std::memory_order_release);
}

void Receiver::close_fd() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

int Receiver::poll_once() noexcept {
    if (fd_ < 0) return 0;
    sockaddr_in src{};
    socklen_t slen = sizeof(src);
    const ssize_t n = ::recvfrom(fd_, buf_, sizeof(buf_), MSG_DONTWAIT,
                                 reinterpret_cast<sockaddr*>(&src), &slen);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        stats_.bad.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    if (static_cast<std::size_t>(n) < sizeof(PktHdr)) {
        stats_.bad.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    PktHdr hdr{};
    std::memcpy(&hdr, buf_, sizeof(hdr));
    if (bswap32(hdr.magic_be) != kMagic || hdr.version != kVersion) {
        stats_.bad.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    const uint8_t count = hdr.count;
    std::size_t off = sizeof(PktHdr);
    const uint64_t recv_t = now_ns();
    int decoded = 0;
    for (uint8_t i = 0; i < count; ++i) {
        Decoded d;
        const std::size_t used = unpack_msg(buf_ + off, static_cast<std::size_t>(n) - off, d);
        if (used == 0) {
            stats_.bad.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        off += used;
        stats_.last = d;
        const uint32_t idx = stats_.n_rtt.fetch_add(1, std::memory_order_relaxed);
        if (idx < kHist && recv_t >= d.event_ns) stats_.rtt_ns[idx] = recv_t - d.event_ns;
        ++decoded;
    }
    stats_.packets.fetch_add(1, std::memory_order_relaxed);
    stats_.messages.fetch_add(static_cast<uint64_t>(decoded), std::memory_order_relaxed);
    return decoded;
}

void Receiver::run_loop() {
    pin_current_thread(cfg_.pin_cpu);
    while (run_.load(std::memory_order_acquire)) {
        if (poll_once() == 0) {
#if defined(__x86_64__)
            __asm__ volatile("pause");
#elif defined(__aarch64__)
            __asm__ volatile("yield");
#endif
        }
    }
}

} // namespace md
