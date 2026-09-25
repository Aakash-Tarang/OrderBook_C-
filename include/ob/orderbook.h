#pragma once
/*
 * orderbook.h – Limit order book with price-time priority.
 *
 * Sixth-phase architecture (evolved from the fourth/fifth iterations):
 *
 *   price discovery : two-level hierarchical bitset per side
 *                     (find best price by CTZ/CLZ over 64-tick words,
 *                      summary layer skips empty words – no linear scan)
 *   order storage   : pre-allocated pool of intrusive doubly-linked nodes
 *                     (no heap allocation after construction)
 *   id lookup       : ob::FlatIdMap (open addressing, backshift deletion)
 *                     – replaces std::unordered_map, which allocated a node
 *                     on every add_order() and freed on every cancel/fill
 *   trade output    : caller-owned reusable std::vector<TradeReport>&
 *                     – replaces returning a fresh vector per call
 *
 * Complexity (amortised, steady state):
 *   add_order(LIMIT)   O(1) lookup + O(1) insert + matching
 *   add_order(MARKET)  matching only
 *   cancel_order       O(1) expected
 *   replace_order      cancel + re-insert (loses time priority, as on
 *                      most exchanges)
 *   best_bid/ask       O(1)
 *
 * Matching rules:
 *   - price-time priority: FIFO within a price level, best price first
 *   - a resting order keeps its position until fully filled or cancelled
 *   - trade price = resting order's price (taker pays the spread)
 *   - order id 0 is reserved and rejected (FlatIdMap empty-slot sentinel)
 *
 * Memory model: single-threaded.  The matching engine thread must be the
 * only user of this object (the SPSC pipeline guarantees that).
 */

#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "flat_id_map.h"

namespace ob {

enum class Side      : uint8_t { BID, ASK };
enum class OrderType : uint8_t { LIMIT, MARKET };

struct Order {
    uint64_t  id    = 0;   // must be non-zero and unique among live orders
    Side      side  = Side::BID;
    int       price = 0;   // integer ticks
    int       qty   = 0;   // > 0
    OrderType type  = OrderType::LIMIT;
};

struct TradeReport {
    uint64_t bid_id;
    uint64_t ask_id;
    int      price;
    int      qty;
};

// ── 2-Level Hierarchical Bitset ──────────────────────────────────────────────
//
// Layer 0  words_[w]        : 1 bit per price tick
// Layer 1  summary_[w >> 6] : 1 bit per Layer-0 word
//
// A summary bit S[k] is 1 iff words_[k] != 0, so find_next_set/find_prev_set
// skip 64 words (4096 ticks) per summary-scan iteration instead of one word
// at a time.  This is the fix for the phase-4 sparse-workload tail latency
// documented in the report §5.7.
//
// Memory overhead: ceil(n_words/64) extra uint64_t values
// (200 000 ticks → 3 125 base words → 49 summary words → 392 bytes).
//
class TickBitset {
public:
    explicit TickBitset(int n_bits)
        : n_bits_(n_bits),
          words_(static_cast<std::size_t>((n_bits + 63) / 64), 0ULL),
          summary_(
              (static_cast<std::size_t>((n_bits + 63) / 64) + 63) / 64,
              0ULL)
    {}

    // Write path – two unconditional OR stores, fully branchless.  The
    // summary bit is always set (OR is idempotent; a "was it set?" branch
    // would cost more than the store).
    inline void set(int idx) noexcept {
        const std::size_t word_idx = static_cast<std::size_t>(idx) >> 6;
        const std::size_t sum_word = word_idx >> 6;
        const std::size_t sum_bit  = word_idx & 63u;

        words_[word_idx]   |= (1ULL << (static_cast<unsigned>(idx) & 63u));
        summary_[sum_word] |= (1ULL << sum_bit);
    }

    // Clear path – summary bit is cleared only when the whole word empties.
    inline void clear(int idx) noexcept {
        const std::size_t word_idx = static_cast<std::size_t>(idx) >> 6;
        const std::size_t sum_word = word_idx >> 6;
        const std::size_t sum_bit  = word_idx & 63u;

        words_[word_idx] &= ~(1ULL << (static_cast<unsigned>(idx) & 63u));

        if (words_[word_idx] == 0ULL)
            summary_[sum_word] &= ~(1ULL << sum_bit);
    }

    inline bool test(int idx) const noexcept {
        return (words_[static_cast<std::size_t>(idx) >> 6]
                >> (static_cast<unsigned>(idx) & 63u)) & 1ULL;
    }

    int find_next_set(int from_idx) const noexcept; // smallest set bit ≥ from
    int find_prev_set(int from_idx) const noexcept; // largest  set bit ≤ from

    int n_bits() const noexcept { return n_bits_; }

private:
    int                   n_bits_;
    std::vector<uint64_t> words_;    // Layer 0
    std::vector<uint64_t> summary_;  // Layer 1
};

struct PriceLevel {
    int32_t head_idx  = -1;   // FIFO head: highest time priority
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
    // price range [price_min, price_max] is fixed at construction;
    // pool_capacity pre-allocates the order-node pool (never grows).
    OrderBook(int price_min, int price_max, std::size_t pool_capacity);

    // ── Core operations ──────────────────────────────────────────────────
    //
    // Trades produced by the operation are appended to `trades_out`
    // (which is NOT cleared first, so callers can batch several operations
    // into one buffer).  Pass a member vector that you clear() per batch:
    // steady-state add_order() then performs zero heap allocations.
    // Returns false when the order is rejected: id 0, qty ≤ 0, price outside
    // the configured range, duplicate live id, or pool exhausted.
    bool add_order(const Order& order, std::vector<TradeReport>& trades_out);
    bool cancel_order(uint64_t order_id);

    // Cancel + re-insert at (new_price, new_qty) with a new position at the
    // back of the level (time priority is lost, matching most exchange
    // behaviours).  Returns false if the original id is not live.
    bool replace_order(uint64_t order_id, int new_price, int new_qty,
                       std::vector<TradeReport>& trades_out);

    // Convenience overload: fresh vector per call (tests, demos, tools –
    // avoid in the hot path).
    std::vector<TradeReport> add_order(const Order& order);

    // ── Market state ─────────────────────────────────────────────────────
    int  best_bid() const;   // INT_MIN when no bids
    int  best_ask() const;   // INT_MAX when no asks
    bool empty_bids() const { return best_bid_idx_ == -1; }
    bool empty_asks() const { return best_ask_idx_ == -1; }
    std::size_t live_orders() const { return id_to_pool_idx_.size(); }

    // ── Test / debug surface ─────────────────────────────────────────────
    //
    // verify_invariants: book not crossed; level total_qty equals the sum of
    // its member orders; bid/ask bitset bits exactly match non-empty levels;
    // best-price indices consistent; pool free-list + map consistent;
    // intrusive links consistent (every next has matching prev, heads/tails
    // correct, no orphan nodes).
    bool verify_invariants(std::string& err_out) const;

    // Levels of one side as (price, total_qty), best price first.
    std::vector<std::pair<int, int>> dump_levels(Side side) const;

    // FIFO order ids at one price level, highest priority first.
    std::vector<uint64_t> dump_level_orders(Side side, int price) const;

private:
    int price_min_;
    int price_max_;
    int n_levels_;

    std::vector<PriceLevel> bid_levels_;
    std::vector<PriceLevel> ask_levels_;
    TickBitset              bid_active_;
    TickBitset              ask_active_;

    std::vector<OrderNode> pool_;
    std::vector<int32_t>   free_list_;
    FlatIdMap              id_to_pool_idx_;

    int best_bid_idx_ = -1;
    int best_ask_idx_ = -1;

    int to_index(int price) const noexcept { return price - price_min_; }

    int32_t alloc_node();
    void    free_node(int32_t idx);

    void insert_limit(const Order& order);
    void match(Side taker_side, std::vector<TradeReport>& trades_out);
    void sweep_book(Side taker_side, int remaining, uint64_t taker_id,
                    std::vector<TradeReport>& trades_out);
    void remove_level_if_empty(Side side, int idx);

    std::vector<PriceLevel>&       levels_for(Side side) noexcept;
    const std::vector<PriceLevel>& levels_for(Side side) const noexcept;
    TickBitset&                    active_for(Side side) noexcept;
    const TickBitset&              active_for(Side side) const noexcept;
};

} // namespace ob
