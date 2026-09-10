#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// Fixed-size ring of per-op latencies (nanoseconds). Oldest samples are
// overwritten once full so a long run keeps a recent window.
template <size_t N>
struct LatencyRing {
    static_assert(N > 0 && (N & (N - 1)) == 0, "N must be a power of two");

    void push(uint64_t ns) noexcept {
        buf_[head_ & (N - 1)] = ns;
        ++head_;
        if (filled_ < N) ++filled_;
    }

    size_t size() const noexcept { return filled_; }

    struct Pct {
        double p50 = 0;
        double p90 = 0;
        double p99 = 0;
        double p999 = 0;
    };

    Pct percentiles() const {
        Pct out;
        if (filled_ == 0) return out;
        std::vector<uint64_t> tmp(filled_);
        const size_t start = (filled_ < N) ? 0 : (head_ - N);
        for (size_t i = 0; i < filled_; ++i) {
            tmp[i] = buf_[(start + i) & (N - 1)];
        }
        std::sort(tmp.begin(), tmp.end());
        auto at = [&](double q) -> double {
            if (tmp.size() == 1) return static_cast<double>(tmp[0]);
            double idx = q * static_cast<double>(tmp.size() - 1);
            size_t lo = static_cast<size_t>(idx);
            size_t hi = std::min(lo + 1, tmp.size() - 1);
            double frac = idx - static_cast<double>(lo);
            return static_cast<double>(tmp[lo]) * (1.0 - frac) +
                   static_cast<double>(tmp[hi]) * frac;
        };
        out.p50 = at(0.50);
        out.p90 = at(0.90);
        out.p99 = at(0.99);
        out.p999 = at(0.999);
        return out;
    }

private:
    std::array<uint64_t, N> buf_{};
    size_t head_ = 0;
    size_t filled_ = 0;
};
