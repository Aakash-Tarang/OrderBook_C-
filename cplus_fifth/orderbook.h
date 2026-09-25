

#pragma once
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace p4 {
enum class Side      { BID, ASK };
enum class OrderType { LIMIT, MARKET };

struct Order {
    uint64_t  id;
    Side      side;
    int       price;
    int       qty;
    OrderType type;
};

struct TradeReport {
    uint64_t bid_id;
    uint64_t ask_id;
    int      price;
    int      qty;
};

// ── 2-Level Hierarchical Bitset ───────────────────────────────────────────────
//
// Public interface: identical to the original TickBitset.
//
// Internal layout
// ───────────────
//   Layer 0  words_[w]          : 1 bit per price tick (unchanged).
//   Layer 1  summary_[w >> 6]   : 1 bit per Layer-0 word.
//
// A summary bit S[k] is 1 iff words_[k] != 0.
// This lets find_next_set / find_prev_set skip 64 ticks per intrinsic call
// instead of 1 word per loop iteration.
//
// Memory overhead: ceil(n_words / 64) extra uint64_t values.
// For 200 000 ticks → 3 125 base words → 49 summary words → 392 extra bytes.
//
class TickBitset {
public:
    // ── Constructor ──────────────────────────────────────────────────────
    explicit TickBitset(int n_bits)
        : n_bits_(n_bits),
          // Layer 0: one bit per tick, rounded up to 64-bit words.
          words_(static_cast<std::size_t>((n_bits + 63) / 64), 0ULL),
          // Layer 1: one bit per Layer-0 word, rounded up to 64-bit words.
          // n_words = words_.size(); summary needs ceil(n_words / 64) words.
          summary_(
              (static_cast<std::size_t>((n_bits + 63) / 64) + 63) / 64,
              0ULL)
    {}

    // ── set(idx) ─────────────────────────────────────────────────────────
    //
    // Write path – two unconditional OR stores, fully branchless.
    //
    // Layer 0: set bit (idx & 63) in word (idx >> 6).
    // Layer 1: set bit (word_idx & 63) in summary word (word_idx >> 6).
    //          We always set (never branch on "was it already set?") because
    //          OR is idempotent and a branch would cost more than the store.
    //
    inline void set(int idx) noexcept {
        const std::size_t word_idx    = static_cast<std::size_t>(idx) >> 6;
        const std::size_t sum_word    = word_idx >> 6;          // summary word index
        const std::size_t sum_bit     = word_idx & 63u;         // bit within that word

        words_[word_idx]  |=  (1ULL << (static_cast<unsigned>(idx) & 63u));
        summary_[sum_word] |= (1ULL << sum_bit);                // branchless OR
    }
    
    inline void clear(int idx) noexcept {
        const std::size_t word_idx = static_cast<std::size_t>(idx) >> 6;
        const std::size_t sum_word = word_idx >> 6;
        const std::size_t sum_bit  = word_idx & 63u;

        words_[word_idx] &= ~(1ULL << (static_cast<unsigned>(idx) & 63u));

        if (words_[word_idx] == 0ULL) {
            summary_[sum_word] &= ~(1ULL << sum_bit);
        }
    }

    inline bool test(int idx) const noexcept {
        return (words_[static_cast<std::size_t>(idx) >> 6]
                >> (static_cast<unsigned>(idx) & 63u)) & 1ULL;
    }

    int find_next_set(int from_idx) const;
    int find_prev_set(int from_idx) const;

private:
    int                   n_bits_;
    std::vector<uint64_t> words_;    // Layer 0
    std::vector<uint64_t> summary_;  // Layer 1
};

struct PriceLevel {
    int32_t head_idx  = -1;
    int32_t tail_idx  = -1;
    int     total_qty = 0;
};

struct OrderNode {
    uint64_t order_id  = 0;
    Side     side      = Side::BID;
    int      price     = 0;
    int      qty       = 0;
    int      price_idx = -1;
    int32_t  next_idx  = -1;
    int32_t  prev_idx  = -1;
};

class OrderBook {
public:
    OrderBook(int price_min, int price_max, size_t pool_capacity);
    std::vector<TradeReport> add_order(const Order& order);
    bool cancel_order(uint64_t order_id);

    int  best_bid() const;
    int  best_ask() const;
    bool empty_bids() const { return best_bid_idx_ == -1; }
    bool empty_asks() const { return best_ask_idx_ == -1; }

private:
    int price_min_;
    int price_max_;
    int n_levels_;

    std::vector<PriceLevel> bid_levels_;
    std::vector<PriceLevel> ask_levels_;
    TickBitset               bid_active_;
    TickBitset               ask_active_;

    std::vector<OrderNode>                pool_;
    std::vector<int32_t>                  free_list_;
    std::unordered_map<uint64_t, int32_t> id_to_pool_idx_;

    int best_bid_idx_ = -1;
    int best_ask_idx_ = -1;

    int to_index(int price) const { return price - price_min_; }

    int32_t alloc_node();
    void    free_node(int32_t idx);

    void                     insert_limit(const Order& order);
    std::vector<TradeReport> match(Side taker_side);
    void                     remove_level_if_empty(Side side, int idx);
};

} // namespace p4