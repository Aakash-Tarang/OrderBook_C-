#pragma once
/*
 * flat_id_map.h – Open-addressed uint64_t → int32_t map with linear probing
 *                 and backshift deletion.  No heap allocation per operation
 *                 in steady state.
 *
 * Why not std::unordered_map
 * ──────────────────────────
 * The order book previously kept  id → pool index  in a
 * std::unordered_map<uint64_t, int32_t>.  Every add_order() performed a
 * node allocation (bucket list node) and every cancel/fill a free – i.e.
 * the "O(1)" lookup hid a malloc/free pair plus a pointer chase that pulls
 * a different cache line per lookup.  A matching engine that already pools
 * its order nodes should not let its lookup index quietly allocate.
 *
 * Design
 * ──────
 *   - Open addressing, linear probing: all slots live in two flat arrays
 *     (keys_, vals_) → contiguous memory, cache-friendly probes.
 *   - Capacity is a power of two → index = hash & mask (no division).
 *   - Empty is encoded as the reserved key UINT64_MAX.  Order id 0 is also
 *     rejected by the OrderBook, so ids are guaranteed ≠ reserved key.
 *     (A separate state array was rejected: it adds one more cache line of
 *     traffic per probe.)
 *   - Deletion without tombstones via *backshift deletion*: after vacating
 *     a slot, following entries that would probe into the hole are shifted
 *     back one slot, keeping every probe chain intact.  No tombstone
 *     accumulation, no separate erase/rebuild pass.
 *   - Hash: splitmix64 finaliser.  Sequential order ids scatter well.
 *   - Grows ×2 when load factor exceeds 0.7 (kept low: linear probing
 *     degrades with clustering; 0.7 keeps expected probes ≈ 1.7 at hit).
 *   - Never shrinks.  A live order count that collapsed after a burst is
 *     not worth a rehash on the hot path (documented behaviour).
 *
 * Not thread-safe, by design: the matching engine thread is the sole owner.
 */

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ob {

class FlatIdMap {
public:
    static constexpr uint64_t kReservedKey = UINT64_MAX; // encodes empty slot

    explicit FlatIdMap(std::size_t initial_capacity = 1024)
    {
        // Round up to a power of two (at least 16).
        std::size_t cap = 16;
        while (cap < initial_capacity) cap <<= 1;
        allocate(cap);
    }

    std::size_t size() const noexcept { return size_; }
    bool        empty() const noexcept { return size_ == 0; }
    std::size_t capacity() const noexcept { return capacity_; }
    double      load_factor() const noexcept
    {
        return size_ == 0 ? 0.0
                          : static_cast<double>(size_) /
                                static_cast<double>(capacity_);
    }

    // ── Lookup ───────────────────────────────────────────────────────────
    // Returns pointer to the value, or nullptr.  The pointer is invalidated
    // by any subsequent insert/erase/rehash.
    const int32_t* find(uint64_t key) const noexcept
    {
        assert(key != kReservedKey);
        std::size_t i = hash(key) & mask_;
        while (keys_[i] != kReservedKey) {
            if (keys_[i] == key) return &vals_[i];
            i = (i + 1) & mask_;
        }
        return nullptr;
    }

    int32_t* find(uint64_t key) noexcept
    {
        return const_cast<int32_t*>(static_cast<const FlatIdMap*>(this)->find(key));
    }

    bool contains(uint64_t key) const noexcept { return find(key) != nullptr; }

    // ── Insert-or-assign ─────────────────────────────────────────────────
    // Always succeeds (grows the table if needed).
    void insert_or_assign(uint64_t key, int32_t value)
    {
        assert(key != kReservedKey);
        if ((size_ + 1) * 10 >= capacity_ * 7)  // load factor ≥ 0.7
            grow();

        std::size_t i = hash(key) & mask_;
        while (keys_[i] != kReservedKey) {
            if (keys_[i] == key) {
                vals_[i] = value; // assign – no size change
                return;
            }
            i = (i + 1) & mask_;
        }
        keys_[i]  = key;
        vals_[i]  = value;
        ++size_;
    }

    // ── Erase (backshift deletion) ───────────────────────────────────────
    // Returns true if the key was present.
    bool erase(uint64_t key) noexcept
    {
        assert(key != kReservedKey);
        std::size_t i = hash(key) & mask_;

        // 1. Find the key.
        while (keys_[i] != kReservedKey) {
            if (keys_[i] == key) {
                backshift_remove(i);
                --size_;
                return true;
            }
            i = (i + 1) & mask_;
        }
        return false;
    }

    void clear() noexcept
    {
        for (std::size_t i = 0; i < capacity_; ++i) keys_[i] = kReservedKey;
        size_ = 0;
    }

    void reserve(std::size_t n)
    {
        if (n * 10 >= capacity_ * 7) grow_to(n);
    }

private:
    // splitmix64 finaliser – avalanche on cheap sequential inputs.
    static uint64_t hash(uint64_t x) noexcept
    {
        x += 0x9E3779B97F4A7C15ULL;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
        return x ^ (x >> 31);
    }

    void allocate(std::size_t cap)
    {
        capacity_ = cap;
        mask_     = cap - 1;
        keys_.assign(cap, kReservedKey);
        vals_.assign(cap, 0);
        size_ = 0;
    }

    void grow_to(std::size_t at_least)
    {
        std::size_t new_cap = capacity_;
        while ((at_least * 10) >= new_cap * 7) new_cap <<= 1;

        std::vector<uint64_t> old_keys  = std::move(keys_);
        std::vector<int32_t>  old_vals  = std::move(vals_);
        const std::size_t     old_cap   = capacity_;
        const std::size_t     old_size  = size_;

        allocate(new_cap);

        for (std::size_t i = 0; i < old_cap; ++i) {
            if (old_keys[i] == kReservedKey) continue;
            std::size_t j = hash(old_keys[i]) & mask_;
            while (keys_[j] != kReservedKey) j = (j + 1) & mask_;
            keys_[j] = old_keys[i];
            vals_[j] = old_vals[i];
        }
        size_ = old_size;
    }

    void grow() { grow_to(size_ + 1); }

    // ── Backshift deletion ───────────────────────────────────────────────
    //
    // After vacating `hole`, walk forward.  An entry at slot `i` with home
    // position `h = hash(key) & mask` must move into the hole when the hole
    // lies cyclically "on the way" between h and i – formally when
    // (hole - h) mod cap < (i - h) mod cap.  Moving it keeps every probe
    // sequence contiguous, so no tombstone is needed and lookups stay exact.
    void backshift_remove(std::size_t hole) noexcept
    {
        std::size_t i = (hole + 1) & mask_;
        while (keys_[i] != kReservedKey) {
            const std::size_t home = hash(keys_[i]) & mask_;
            // Cyclic distances (both < capacity because mask is cap-1).
            const std::size_t dist_to_hole = (hole + capacity_ - home) & mask_;
            const std::size_t dist_to_i    = (i    + capacity_ - home) & mask_;
            if (dist_to_hole < dist_to_i) {
                keys_[hole] = keys_[i];
                vals_[hole] = vals_[i];
                hole        = i;
            }
            i = (i + 1) & mask_;
        }
        keys_[hole] = kReservedKey;
    }

    std::vector<uint64_t> keys_;
    std::vector<int32_t>  vals_;
    std::size_t           size_     = 0;
    std::size_t           capacity_ = 0;
    std::size_t           mask_     = 0;
};

} // namespace ob
