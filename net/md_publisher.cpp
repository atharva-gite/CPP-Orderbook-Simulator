#include "md_publisher.hpp"
#include "thread_pin.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

namespace md {

namespace {
void cpu_relax() noexcept {
#if defined(__x86_64__)
    __asm__ volatile("pause");
#elif defined(__aarch64__)
    __asm__ volatile("yield");
#endif
}
} // namespace

Publisher::Publisher(const Config& cfg) : cfg_(cfg) {}

Publisher::~Publisher() { stop(); }

bool Publisher::enqueue(const Event& e) noexcept {
    if (!q_.try_push(e)) {
        stats_.dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    stats_.enqueued.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool Publisher::try_publish_add(lob::order_id_t id, lob::Side side, lob::price_t px, lob::qty_t qty,
                                uint64_t event_ns) noexcept {
    Event e;
    e.type = MsgType::Add;
    e.side = side;
    e.order_id = id;
    e.price = px;
    e.qty = qty;
    e.event_ns = event_ns;
    return enqueue(e);
}

bool Publisher::try_publish_cancel(lob::order_id_t id, uint64_t event_ns) noexcept {
    Event e;
    e.type = MsgType::Cancel;
    e.order_id = id;
    e.event_ns = event_ns;
    return enqueue(e);
}

bool Publisher::try_publish_execute(lob::order_id_t id, lob::price_t px, lob::qty_t qty,
                                    uint64_t event_ns) noexcept {
    Event e;
    e.type = MsgType::Execute;
    e.order_id = id;
    e.price = px;
    e.qty = qty;
    e.event_ns = event_ns;
    return enqueue(e);
}

bool Publisher::try_publish_snapshot(const lob::OrderBook& book, uint64_t event_ns) noexcept {
    Event e;
    e.type = MsgType::Snapshot;
    e.event_ns = event_ns;
    if (auto b = book.best_bid()) {
        e.snap_flags |= 1u;
        e.bid = *b;
        e.bid_qty = book.best_bid_qty();
    }
    if (auto a = book.best_ask()) {
        e.snap_flags |= 2u;
        e.ask = *a;
        e.ask_qty = book.best_ask_qty();
    }
    return enqueue(e);
}

void Publisher::record(uint64_t* buf, std::atomic<uint32_t>& n, uint64_t v) noexcept {
    const uint32_t i = n.fetch_add(1, std::memory_order_relaxed);
    if (i < kHist) buf[i] = v;
}

bool Publisher::start() {
    if (run_.load(std::memory_order_relaxed)) return true;
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) return false;

    int loop = cfg_.loopback ? 1 : 0;
    ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
    unsigned char ttl = static_cast<unsigned char>(cfg_.ttl);
    ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    if (cfg_.loopback) {
        in_addr iface{};
        iface.s_addr = htonl(INADDR_LOOPBACK);
        ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_IF, &iface, sizeof(iface));
    }
    int snd = 1 << 20;
    ::setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &snd, sizeof(snd));

    run_.store(true, std::memory_order_release);
    th_ = std::thread([this] { thread_main(); });
    return true;
}

void Publisher::stop() {
    if (!run_.exchange(false, std::memory_order_acq_rel)) {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        return;
    }
    if (th_.joinable()) th_.join();
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

void Publisher::thread_main() {
    pin_current_thread(cfg_.pin_cpu);

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(cfg_.port);
    if (::inet_pton(AF_INET, cfg_.group, &dst.sin_addr) != 1) return;

    uint8_t pkt[kUdpPayload];

    while (run_.load(std::memory_order_acquire)) {
        std::size_t off = sizeof(PktHdr);
        uint8_t count = 0;
        uint64_t ev_ns[64];

        const uint64_t ser0 = now_ns();
        Event ev;
        while (count < 64 && off + kMaxMsgBytes <= kUdpPayload && q_.try_pop(ev)) {
            const std::size_t n = pack_msg(pkt + off, ev);
            if (n == 0) break;
            ev_ns[count] = ev.event_ns;
            off += n;
            ++count;
        }
        const uint64_t ser1 = now_ns();

        if (count == 0) {
            cpu_relax();
            continue;
        }

        const uint64_t ser_batch = ser1 - ser0;
        const uint64_t ser_each = ser_batch / static_cast<uint64_t>(count);
        for (uint8_t i = 0; i < count; ++i) {
            record(stats_.ser_ns, stats_.n_ser, ser_each);
        }

        PktHdr hdr{};
        hdr.magic_be = bswap32(kMagic);
        hdr.version = kVersion;
        hdr.count = count;
        hdr.nbytes_be = bswap16(static_cast<uint16_t>(off - sizeof(PktHdr)));
        hdr.pkt_seq_be = bswap64(pkt_seq_++);
        hdr.send_ns_be = bswap64(ser1);
        std::memcpy(pkt, &hdr, sizeof(hdr));

        const ssize_t sent = ::sendto(fd_, pkt, static_cast<socklen_t>(off), 0,
                                      reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
        const uint64_t after_send = now_ns();
        if (sent < 0 || static_cast<std::size_t>(sent) != off) {
            stats_.send_fail.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        stats_.packets.fetch_add(1, std::memory_order_relaxed);
        stats_.messages.fetch_add(count, std::memory_order_relaxed);
        for (uint8_t i = 0; i < count; ++i) {
            record(stats_.pub_ns, stats_.n_pub, after_send - ev_ns[i]);
        }
    }
}

} // namespace md
