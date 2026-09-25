// TickBitset unit tests – hierarchical bitset vs a naive std::vector<char>.
#include "test_framework.h"

#include <random>
#include <vector>

#include "ob/orderbook.h"

namespace {

// Naive reference for the bitset semantics.
struct NaiveBits {
    std::vector<char> v;
    explicit NaiveBits(int n) : v(static_cast<size_t>(n), 0) {}

    void set(int i)   { v[static_cast<size_t>(i)] = 1; }
    void clear(int i) { v[static_cast<size_t>(i)] = 0; }
    bool test(int i) const { return v[static_cast<size_t>(i)] != 0; }

    int next(int from) const
    {
        for (int i = from < 0 ? 0 : from; i < static_cast<int>(v.size()); ++i)
            if (v[static_cast<size_t>(i)]) return i;
        return -1;
    }
    int prev(int from) const
    {
        if (from >= static_cast<int>(v.size())) from = static_cast<int>(v.size()) - 1;
        for (int i = from; i >= 0; --i)
            if (v[static_cast<size_t>(i)]) return i;
        return -1;
    }
};

} // namespace

OB_TEST(bitset_empty_finds_nothing)
{
    ob::TickBitset bs(1024);
    OB_CHECK(bs.find_next_set(0) == -1);
    OB_CHECK(bs.find_prev_set(1023) == -1);
}

OB_TEST(bitset_single_bits_boundaries)
{
    const int n = 4096;
    ob::TickBitset bs(n);
    NaiveBits      ref(n);

    // Set first and last bits only.
    bs.set(0);    ref.set(0);
    bs.set(n - 1); ref.set(n - 1);

    OB_CHECK(bs.test(0) == ref.test(0));
    OB_CHECK(bs.test(n - 1) == ref.test(n - 1));
    OB_CHECK(bs.find_next_set(0) == 0);
    OB_CHECK(bs.find_next_set(1) == n - 1);
    OB_CHECK(bs.find_prev_set(n - 1) == n - 1);
    OB_CHECK(bs.find_prev_set(n - 2) == 0);

    // Clear the last bit; everything must collapse to index 0 / nothing.
    bs.clear(n - 1); ref.clear(n - 1);
    OB_CHECK(bs.find_next_set(1) == -1);
    OB_CHECK(bs.find_prev_set(n - 1) == 0);
    bs.clear(0); ref.clear(0);
    OB_CHECK(bs.find_next_set(0) == -1);
    OB_CHECK(bs.find_prev_set(n - 1) == -1);
    OB_CHECK(!bs.test(0));
}

OB_TEST(bitset_word_boundaries)
{
    // 64-bit word edges: bits 62, 63, 64, 65 must behave independently.
    ob::TickBitset bs(200);
    NaiveBits      ref(200);
    const int probes[] = {62, 63, 64, 65, 127, 128, 129};
    for (int p : probes) { bs.set(p); ref.set(p); }
    for (int p : probes) OB_CHECK(bs.test(p));

    for (int from = 0; from < 200; ++from)
        OB_CHECK(bs.find_next_set(from) == ref.next(from));
    for (int from = 199; from >= 0; --from)
        OB_CHECK(bs.find_prev_set(from) == ref.prev(from));
}

OB_TEST(bitset_randomized_against_naive)
{
    std::mt19937_64 rng(1234);
    for (int trial = 0; trial < 20; ++trial) {
        const int n = 1 + static_cast<int>(rng() % 5000);
        ob::TickBitset bs(n);
        NaiveBits      ref(n);

        for (int step = 0; step < 4000; ++step) {
            const int op = static_cast<int>(rng() % 4);
            const int i  = static_cast<int>(rng() % static_cast<unsigned>(n));
            switch (op) {
            case 0: case 1: bs.set(i); ref.set(i); break;
            case 2: bs.clear(i); ref.clear(i); break;
            case 3: OB_CHECK(bs.test(i) == ref.test(i)); break;
            }

            // Randomised range queries, including out-of-range clamps.
            const int from_next = static_cast<int>(rng() % static_cast<unsigned>(n + 4)) - 2;
            const int from_prev = static_cast<int>(rng() % static_cast<unsigned>(n + 4)) - 2;
            OB_CHECK(bs.find_next_set(from_next) == ref.next(from_next));
            OB_CHECK(bs.find_prev_set(from_prev) == ref.prev(from_prev));
        }

        // Full sweep equality after the storm.
        for (int from = 0; from < n; ++from) {
            OB_CHECK(bs.find_next_set(from) == ref.next(from));
            OB_CHECK(bs.find_prev_set(from) == ref.prev(from));
        }
    }
}
