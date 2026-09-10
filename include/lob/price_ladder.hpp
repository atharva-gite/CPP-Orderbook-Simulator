#pragma once

#include "lob/types.hpp"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace lob {

// Contiguous price ladder. Typical simulator depth is ~100 ticks/side.
// Requires PriceLevel to already be defined (included from order_book.hpp).
template <typename Cmp>
class PriceLadder {
public:
    struct Node {
        price_t price{};
        PriceLevel level;
    };

    using iterator = typename std::vector<Node>::iterator;
    using const_iterator = typename std::vector<Node>::const_iterator;

    iterator begin() noexcept { return v_.begin(); }
    iterator end() noexcept { return v_.end(); }
    const_iterator begin() const noexcept { return v_.begin(); }
    const_iterator end() const noexcept { return v_.end(); }

    bool empty() const noexcept { return v_.empty(); }
    std::size_t size() const noexcept { return v_.size(); }
    void reserve(std::size_t n) { v_.reserve(n); }

    iterator find(price_t px) {
        auto it = lower(px);
        if (it == v_.end() || it->price != px) return v_.end();
        return it;
    }
    const_iterator find(price_t px) const {
        auto it = lower(px);
        if (it == v_.end() || it->price != px) return v_.end();
        return it;
    }

    iterator find_or_emplace(price_t px) {
        auto it = lower(px);
        if (it != v_.end() && it->price == px) return it;
        return v_.insert(it, Node{px, PriceLevel{}});
    }

    iterator erase(iterator it) { return v_.erase(it); }

    iterator erase_price(price_t px) {
        auto it = find(px);
        if (it == v_.end()) return v_.end();
        return v_.erase(it);
    }

private:
    std::vector<Node> v_;
    Cmp cmp_{};

    iterator lower(price_t px) {
        return std::lower_bound(v_.begin(), v_.end(), px,
                                [&](const Node& n, price_t p) { return cmp_(n.price, p); });
    }
    const_iterator lower(price_t px) const {
        return std::lower_bound(v_.begin(), v_.end(), px,
                                [&](const Node& n, price_t p) { return cmp_(n.price, p); });
    }
};

} // namespace lob
