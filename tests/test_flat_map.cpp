// FlatIdMap unit tests – against std::unordered_map as the oracle.
#include "test_framework.h"

#include <cstdint>
#include <random>
#include <unordered_map>

#include "ob/flat_id_map.h"

OB_TEST(flat_map_basic_insert_find_erase)
{
    ob::FlatIdMap m(16);
    OB_CHECK(m.empty());
    OB_CHECK(m.find(1) == nullptr);

    m.insert_or_assign(1, 100);
    m.insert_or_assign(2, 200);
    OB_CHECK(m.size() == 2);

    const int32_t* v = m.find(1);
    OB_REQUIRE(v != nullptr);
    OB_CHECK(*v == 100);

    v = m.find(2);
    OB_REQUIRE(v != nullptr);
    OB_CHECK(*v == 200);
    OB_CHECK(m.find(3) == nullptr);

    OB_CHECK(m.erase(1));
    OB_CHECK(m.find(1) == nullptr);
    OB_CHECK(!m.erase(1));          // second erase fails
    OB_CHECK(m.size() == 1);
    OB_CHECK(m.find(2) != nullptr); // neighbour survived the backshift
}

OB_TEST(flat_map_insert_or_assign_overwrites)
{
    ob::FlatIdMap m(16);
    m.insert_or_assign(7, 1);
    m.insert_or_assign(7, 42);
    OB_CHECK(m.size() == 1);
    OB_CHECK(*m.find(7) == 42);
}

OB_TEST(flat_map_reserved_key_rejected_by_assert_in_debug)
{
    // The reserved key is asserted against; in release builds inserting it
    // is undefined behaviour, so we only verify the constant's value.
    OB_CHECK(ob::FlatIdMap::kReservedKey == UINT64_MAX);
}

OB_TEST(flat_map_grows_and_keeps_data)
{
    ob::FlatIdMap m(16);
    std::unordered_map<uint64_t, int32_t> ref;

    for (uint64_t k = 1; k <= 5000; ++k) {
        const int32_t val = static_cast<int32_t>(k * 3);
        m.insert_or_assign(k, val);
        ref[k] = val;
    }
    OB_CHECK(m.size() == ref.size());
    OB_CHECK(m.capacity() >= 5000); // actually grew

    for (const auto& kv : ref) {
        const int32_t* v = m.find(kv.first);
        OB_REQUIRE(v != nullptr);
        OB_CHECK(*v == kv.second);
    }
}

OB_TEST(flat_map_randomized_vs_unordered_map)
{
    std::mt19937_64 rng(99);
    ob::FlatIdMap m(8);
    std::unordered_map<uint64_t, int32_t> ref;

    uint64_t next_id = 1;
    for (int step = 0; step < 50000; ++step) {
        const int op = static_cast<int>(rng() % 3);
        if (op == 0) { // insert / assign
            const uint64_t k = (rng() % 8 == 0)
                ? (next_id++)                     // fresh key
                : (1 + rng() % 2000);             // likely existing key
            const int32_t v = static_cast<int32_t>(rng() % 100000);
            m.insert_or_assign(k, v);
            ref[k] = v;
        } else if (op == 1) { // erase
            const uint64_t k = 1 + rng() % 2000;
            const bool a = m.erase(k);
            const bool b = ref.erase(k) > 0;
            OB_CHECK(a == b);
        } else { // find
            const uint64_t k = 1 + rng() % 2000;
            const int32_t* av = m.find(k);
            auto              it = ref.find(k);
            if (it == ref.end()) {
                OB_CHECK(av == nullptr);
            } else {
                OB_REQUIRE(av != nullptr);
                OB_CHECK(*av == it->second);
            }
        }
        OB_CHECK(m.size() == ref.size());
    }
}

OB_TEST(flat_map_erase_all_then_reuse)
{
    ob::FlatIdMap m(64);
    for (uint64_t k = 1; k <= 1000; ++k) m.insert_or_assign(k, static_cast<int32_t>(k));
    for (uint64_t k = 1; k <= 1000; ++k) OB_CHECK(m.erase(k));
    OB_CHECK(m.empty());

    // The table must be fully reusable after mass deletion (backshift kept
    // every probe chain intact).
    for (uint64_t k = 1001; k <= 2000; ++k) m.insert_or_assign(k, static_cast<int32_t>(k));
    for (uint64_t k = 1001; k <= 2000; ++k) {
        const int32_t* v = m.find(k);
        OB_REQUIRE(v != nullptr);
        OB_CHECK(*v == static_cast<int32_t>(k));
    }
    OB_CHECK(m.size() == 1000);
}

OB_TEST(flat_map_clear)
{
    ob::FlatIdMap m(32);
    for (uint64_t k = 1; k <= 100; ++k) m.insert_or_assign(k, 7);
    m.clear();
    OB_CHECK(m.empty());
    for (uint64_t k = 1; k <= 100; ++k) OB_CHECK(m.find(k) == nullptr);
    m.insert_or_assign(55, 9);
    OB_CHECK(*m.find(55) == 9);
}
