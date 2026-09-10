#include <catch2/catch_test_macros.hpp>
#include "lob/order_pool.hpp"
#include <vector>
#include <atomic>
#include <cstddef>
#include <new>

// Global allocation counter overrides to detect heap allocations
static std::atomic<int> g_alloc_count{0};

void* operator new(std::size_t sz) {
    ++g_alloc_count;
    if (void* p = std::malloc(sz)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept {
    std::free(p);
}

void* operator new(std::size_t sz, std::align_val_t align) {
    ++g_alloc_count;
    void* p = nullptr;
    if (posix_memalign(&p, static_cast<std::size_t>(align), sz) == 0) return p;
    throw std::bad_alloc();
}
void operator delete(void* p, std::align_val_t) noexcept {
    std::free(p);
}

TEST_CASE("OrderPool alloc/free 1M without extra heap allocs", "[order_pool][alloc]"){
    const size_t N = 1000000;
    // Create pool (this will allocate and increment counter)
    {
        lob::OrderPool pool(N);
        std::vector<lob::Order*> holders;
        holders.reserve(N); // reserve holder storage before measuring

        // Reset counter after initial reservations (pool + holders)
        g_alloc_count.store(0);

        for (size_t i = 0; i < N; ++i) {
            auto* o = pool.allocate();
            REQUIRE(o != nullptr);
            holders.push_back(o);
        }

        for (size_t i = 0; i < N; ++i) {
            pool.deallocate(holders[i]);
        }

        // After allocation/free loop there should be no additional heap allocs
        REQUIRE(g_alloc_count.load() == 0);
    }
}
