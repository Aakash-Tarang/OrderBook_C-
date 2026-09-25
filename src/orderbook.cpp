#include "ob/orderbook.h"

#include <algorithm>
#include <cassert>

namespace ob {

// ─────────────────────────────────────────────────────────────────────────────
// TickBitset::find_next_set – smallest set bit ≥ from_idx
//
//   Phase 1: scan within the starting base word (mask bits below from_idx,
//            one CTZ finds the answer).
//   Phase 2: summary-layer scan (each summary word covers 64 base words =
//            4096 ticks; the loop advances 4096 ticks per iteration, which
//            is what removes the phase-4 sparse-workload linear scan).
//   Phase 3: direct jump into the base word guaranteed non-zero by the
//            summary bit, one CTZ finishes.
// ─────────────────────────────────────────────────────────────────────────────
int TickBitset::find_next_set(int from_idx) const noexcept
{
    if (from_idx < 0)        from_idx = 0;
    if (from_idx >= n_bits_) return -1;

    const std::size_t n_base_words = words_.size();

    // ── Phase 1: scan within the starting base word ───────────────────────
    std::size_t word_idx = static_cast<std::size_t>(from_idx) >> 6;
    const int   bit_pos  = from_idx & 63;

    uint64_t w = words_[word_idx] & (~0ULL << static_cast<unsigned>(bit_pos));
    if (w != 0ULL) {
        int found = static_cast<int>(word_idx * 64ULL)
                    + static_cast<int>(__builtin_ctzll(w));
        return (found < n_bits_) ? found : -1;
    }

    // Phase 1 exhausted → move to the next base word.
    ++word_idx;
    if (word_idx >= n_base_words) return -1;

    // ── Phase 2: summary-layer scan, skipping empty base words ────────────
    std::size_t sum_word_idx = word_idx >> 6;
    const std::size_t n_sum_words = summary_.size();

    // Mask away summary bits for base words we already visited.
    const unsigned sum_bit_pos = static_cast<unsigned>(word_idx & 63ULL);
    uint64_t sw = summary_[sum_word_idx] & (~0ULL << sum_bit_pos);

    while (sw == 0ULL) {
        ++sum_word_idx;
        if (sum_word_idx >= n_sum_words) return -1;
        sw = summary_[sum_word_idx];
    }

    // ── Phase 3: jump directly to the non-empty base word ─────────────────
    const std::size_t base_word_target =
        (sum_word_idx << 6) + static_cast<std::size_t>(__builtin_ctzll(sw));

    if (base_word_target >= n_base_words) return -1;

    const int found =
        static_cast<int>(base_word_target * 64ULL)
        + static_cast<int>(__builtin_ctzll(words_[base_word_target]));

    return (found < n_bits_) ? found : -1;
}

// ─────────────────────────────────────────────────────────────────────────────
// find_prev_set – largest set bit ≤ from_idx (mirror image of find_next_set)
// ─────────────────────────────────────────────────────────────────────────────
int TickBitset::find_prev_set(int from_idx) const noexcept
{
    if (from_idx >= n_bits_) from_idx = n_bits_ - 1;
    if (from_idx < 0)        return -1;

    // ── Phase 1: scan within the starting base word ───────────────────────
    std::size_t word_idx = static_cast<std::size_t>(from_idx) >> 6;
    const int   bit_pos  = from_idx & 63;

    // Keep only bits [0 .. bit_pos].  Special-case bit_pos == 63 because
    // 1ULL << 64 would be UB.
    const uint64_t mask =
        (bit_pos == 63) ? ~0ULL
                        : ((1ULL << static_cast<unsigned>(bit_pos + 1)) - 1ULL);

    uint64_t w = words_[word_idx] & mask;
    if (w != 0ULL) {
        return static_cast<int>(word_idx * 64ULL)
               + static_cast<int>(63 - __builtin_clzll(w));
    }

    if (word_idx == 0) return -1;
    --word_idx;

    // ── Phase 2: summary-layer scan backwards ─────────────────────────────
    std::size_t sum_word_idx = word_idx >> 6;

    const unsigned sum_bit_pos = static_cast<unsigned>(word_idx & 63ULL);
    const uint64_t sum_mask =
        (sum_bit_pos == 63) ? ~0ULL
                            : ((1ULL << (sum_bit_pos + 1u)) - 1ULL);

    uint64_t sw = summary_[sum_word_idx] & sum_mask;

    while (sw == 0ULL) {
        if (sum_word_idx == 0) return -1;
        --sum_word_idx;
        sw = summary_[sum_word_idx]; // take all bits of the previous word
    }

    // ── Phase 3: jump directly to the non-empty base word ─────────────────
    const std::size_t base_word_target =
        (sum_word_idx << 6)
        + static_cast<std::size_t>(63 - __builtin_clzll(sw));

    const int found =
        static_cast<int>(base_word_target * 64ULL)
        + static_cast<int>(63 - __builtin_clzll(words_[base_word_target]));

    return (found < n_bits_) ? found : -1;
}

// ── Construction / pool ──────────────────────────────────────────────────────

OrderBook::OrderBook(int price_min, int price_max, std::size_t pool_capacity)
    : price_min_(price_min),
      price_max_(price_max),
      n_levels_(price_max - price_min + 1),
      bid_levels_(static_cast<std::size_t>(n_levels_)),
      ask_levels_(static_cast<std::size_t>(n_levels_)),
      bid_active_(n_levels_),
      ask_active_(n_levels_),
      pool_(pool_capacity)
{
    assert(pool_capacity > 0);
    free_list_.reserve(pool_capacity);
    for (std::size_t i = pool_capacity; i-- > 0; )
        free_list_.push_back(static_cast<int32_t>(i));
    id_to_pool_idx_.reserve(pool_capacity / 2);
}

int32_t OrderBook::alloc_node()
{
    if (free_list_.empty()) return -1; // pool exhausted – caller rejects
    int32_t idx = free_list_.back();
    free_list_.pop_back();
    return idx;
}

void OrderBook::free_node(int32_t idx)
{
    pool_[static_cast<std::size_t>(idx)] = OrderNode{};
    free_list_.push_back(idx);
}

// ── Side helpers ─────────────────────────────────────────────────────────────

std::vector<PriceLevel>& OrderBook::levels_for(Side side) noexcept
{ return (side == Side::BID) ? bid_levels_ : ask_levels_; }

const std::vector<PriceLevel>& OrderBook::levels_for(Side side) const noexcept
{ return (side == Side::BID) ? bid_levels_ : ask_levels_; }

TickBitset& OrderBook::active_for(Side side) noexcept
{ return (side == Side::BID) ? bid_active_ : ask_active_; }

const TickBitset& OrderBook::active_for(Side side) const noexcept
{ return (side == Side::BID) ? bid_active_ : ask_active_; }

// ── Insert / cancel ──────────────────────────────────────────────────────────

void OrderBook::insert_limit(const Order& order)
{
    const int idx = to_index(order.price);
    auto& levels = levels_for(order.side);
    auto& active = active_for(order.side);
    PriceLevel& level = levels[static_cast<std::size_t>(idx)];

    const int32_t node_idx = alloc_node();
    assert(node_idx != -1); // callers must have checked pool space
    OrderNode& node = pool_[static_cast<std::size_t>(node_idx)];

    node.order_id  = order.id;
    node.side      = order.side;
    node.price     = order.price;
    node.qty       = order.qty;
    node.price_idx = idx;
    node.next_idx  = -1;
    node.prev_idx  = level.tail_idx;

    // Append at the tail of the level's FIFO list (time priority).
    if (level.tail_idx != -1)
        pool_[static_cast<std::size_t>(level.tail_idx)].next_idx = node_idx;
    else
        level.head_idx = node_idx;
    level.tail_idx   = node_idx;
    level.total_qty += order.qty;

    active.set(idx);
    id_to_pool_idx_.insert_or_assign(order.id, node_idx);

    if (order.side == Side::BID) {
        if (best_bid_idx_ == -1 || idx > best_bid_idx_) best_bid_idx_ = idx;
    } else {
        if (best_ask_idx_ == -1 || idx < best_ask_idx_) best_ask_idx_ = idx;
    }
}

bool OrderBook::cancel_order(uint64_t order_id)
{
    const int32_t* slot = id_to_pool_idx_.find(order_id);
    if (slot == nullptr) return false;

    const int32_t node_idx = *slot;
    OrderNode&    node     = pool_[static_cast<std::size_t>(node_idx)];

    const Side side = node.side;
    const int  idx  = node.price_idx;
    auto& levels     = levels_for(side);
    PriceLevel& level = levels[static_cast<std::size_t>(idx)];

    // O(1) unlink from the intrusive doubly-linked list.
    if (node.prev_idx != -1)
        pool_[static_cast<std::size_t>(node.prev_idx)].next_idx = node.next_idx;
    else
        level.head_idx = node.next_idx;

    if (node.next_idx != -1)
        pool_[static_cast<std::size_t>(node.next_idx)].prev_idx = node.prev_idx;
    else
        level.tail_idx = node.prev_idx;

    level.total_qty -= node.qty;

    id_to_pool_idx_.erase(order_id);
    free_node(node_idx);

    remove_level_if_empty(side, idx);
    return true;
}

bool OrderBook::replace_order(uint64_t order_id, int new_price, int new_qty,
                              std::vector<TradeReport>& trades_out)
{
    const int32_t* slot = id_to_pool_idx_.find(order_id);
    if (slot == nullptr) return false;

    if (new_qty <= 0)                       return false;
    if (new_price < price_min_ || new_price > price_max_) return false;

    // Validate before mutating: the amend is atomic from the caller's view.
    const Side side = pool_[static_cast<std::size_t>(*slot)].side;

    cancel_order(order_id);
    insert_limit(Order{order_id, side, new_price, new_qty, OrderType::LIMIT});
    match(side, trades_out);
    return true;
}

// ── Best prices ──────────────────────────────────────────────────────────────

int OrderBook::best_bid() const
{
    if (best_bid_idx_ == -1) return std::numeric_limits<int>::min();
    return best_bid_idx_ + price_min_;
}

int OrderBook::best_ask() const
{
    if (best_ask_idx_ == -1) return std::numeric_limits<int>::max();
    return best_ask_idx_ + price_min_;
}

void OrderBook::remove_level_if_empty(Side side, int idx)
{
    auto& levels = levels_for(side);
    auto& active = active_for(side);

    if (levels[static_cast<std::size_t>(idx)].head_idx != -1)
        return; // level still has orders; nothing to do

    active.clear(idx); // updates both Layer 0 and Layer 1

    if (side == Side::BID && idx == best_bid_idx_)
        best_bid_idx_ = bid_active_.find_prev_set(idx - 1);
    if (side == Side::ASK && idx == best_ask_idx_)
        best_ask_idx_ = ask_active_.find_next_set(idx + 1);
}

// ── Matching ─────────────────────────────────────────────────────────────────

void OrderBook::match(Side taker_side, std::vector<TradeReport>& trades_out)
{
    while (best_bid_idx_ != -1 && best_ask_idx_ != -1 &&
           best_bid_idx_ >= best_ask_idx_)
    {
        PriceLevel& bid_level = bid_levels_[static_cast<std::size_t>(best_bid_idx_)];
        PriceLevel& ask_level = ask_levels_[static_cast<std::size_t>(best_ask_idx_)];

        while (bid_level.head_idx != -1 && ask_level.head_idx != -1) {
            const int32_t bid_node_idx = bid_level.head_idx;
            const int32_t ask_node_idx = ask_level.head_idx;
            OrderNode& bid_node = pool_[static_cast<std::size_t>(bid_node_idx)];
            OrderNode& ask_node = pool_[static_cast<std::size_t>(ask_node_idx)];

            // Taker pays the spread: trade prints at the resting order's price.
            const int trade_price =
                (taker_side == Side::BID) ? ask_node.price : bid_node.price;
            const int traded_qty = std::min(bid_node.qty, ask_node.qty);

            bid_node.qty        -= traded_qty;
            ask_node.qty        -= traded_qty;
            bid_level.total_qty -= traded_qty;
            ask_level.total_qty -= traded_qty;

            trades_out.push_back({bid_node.order_id, ask_node.order_id,
                                  trade_price, traded_qty});

            if (bid_node.qty == 0) {
                bid_level.head_idx = bid_node.next_idx;
                if (bid_level.head_idx != -1)
                    pool_[static_cast<std::size_t>(bid_level.head_idx)].prev_idx = -1;
                else
                    bid_level.tail_idx = -1;
                id_to_pool_idx_.erase(bid_node.order_id);
                free_node(bid_node_idx);
            }
            if (ask_node.qty == 0) {
                ask_level.head_idx = ask_node.next_idx;
                if (ask_level.head_idx != -1)
                    pool_[static_cast<std::size_t>(ask_level.head_idx)].prev_idx = -1;
                else
                    ask_level.tail_idx = -1;
                id_to_pool_idx_.erase(ask_node.order_id);
                free_node(ask_node_idx);
            }
        }

        remove_level_if_empty(Side::BID, best_bid_idx_);
        remove_level_if_empty(Side::ASK, best_ask_idx_);
    }
}

void OrderBook::sweep_book(Side taker_side, int remaining, uint64_t taker_id,
                           std::vector<TradeReport>& trades_out)
{
    // Market order sweeps the OPPOSITE side, best price first, FIFO within
    // each level.  Whatever cannot be filled is dropped (documented
    // behaviour; a real engine would emit an ExecType=CALCULATED reject).
    const Side maker_side = (taker_side == Side::BID) ? Side::ASK : Side::BID;
    auto& levels = levels_for(maker_side);

    while (remaining > 0) {
        const int best_idx =
            (taker_side == Side::BID) ? best_ask_idx_ : best_bid_idx_;
        if (best_idx == -1) break;

        PriceLevel& level = levels[static_cast<std::size_t>(best_idx)];

        while (remaining > 0 && level.head_idx != -1) {
            const int32_t node_idx = level.head_idx;
            OrderNode& node = pool_[static_cast<std::size_t>(node_idx)];

            const int traded = std::min(remaining, node.qty);
            remaining        -= traded;
            node.qty         -= traded;
            level.total_qty  -= traded;

            if (taker_side == Side::BID)
                trades_out.push_back({taker_id, node.order_id, node.price, traded});
            else
                trades_out.push_back({node.order_id, taker_id, node.price, traded});

            if (node.qty == 0) {
                level.head_idx = node.next_idx;
                if (level.head_idx != -1)
                    pool_[static_cast<std::size_t>(level.head_idx)].prev_idx = -1;
                else
                    level.tail_idx = -1;
                id_to_pool_idx_.erase(node.order_id);
                free_node(node_idx);
            }
        }
        remove_level_if_empty(maker_side, best_idx);
    }
}

// ── add_order ────────────────────────────────────────────────────────────────

bool OrderBook::add_order(const Order& order,
                          std::vector<TradeReport>& trades_out)
{
    if (order.id == 0)      return false; // reserved id (FlatIdMap sentinel)
    if (order.qty <= 0)     return false; // malformed
    if (order.type == OrderType::LIMIT) {
        if (order.price < price_min_ || order.price > price_max_)
            return false;                     // outside configured tick range
        if (id_to_pool_idx_.find(order.id) != nullptr)
            return false;                     // duplicate live id
        if (free_list_.empty())
            return false;                     // pool exhausted
        insert_limit(order);
        match(order.side, trades_out);
        return true;
    }

    // MARKET order: sweep only (never rests).  Duplicate-id check is
    // irrelevant (nothing is stored) but qty must still be valid.
    sweep_book(order.side, order.qty, order.id, trades_out);
    return true;
}

std::vector<TradeReport> OrderBook::add_order(const Order& order)
{
    std::vector<TradeReport> reports;
    add_order(order, reports);
    return reports;
}

// ── Invariants & dumps (test / debug surface) ────────────────────────────────

bool OrderBook::verify_invariants(std::string& err_out) const
{
    auto fail = [&err_out](const std::string& msg) {
        err_out = msg;
        return false;
    };

    const int  sides        = 2;
    const Side side_ids[2]  = {Side::BID, Side::ASK};
    int  live_nodes         = 0;
    std::size_t pool_nodes_used = 0;

    for (int s = 0; s < sides; ++s) {
        const Side side = side_ids[s];
        const auto& levels = levels_for(side);
        const auto& active = active_for(side);
        const int   best_idx = (side == Side::BID) ? best_bid_idx_ : best_ask_idx_;

        // Every level must agree with its bitset bit.
        for (int idx = 0; idx < n_levels_; ++idx) {
            const PriceLevel& lv = levels[static_cast<std::size_t>(idx)];
            const bool bit = active.test(idx);
            if (lv.head_idx == -1) {
                if (bit)
                    return fail("bitset bit set for empty level "
                                + std::to_string(idx));
                if (lv.tail_idx != -1 || lv.total_qty != 0)
                    return fail("empty level with tail/qty at "
                                + std::to_string(idx));
            } else {
                if (!bit)
                    return fail("non-empty level missing bitset bit "
                                + std::to_string(idx));
            }
        }

        // Best price must match the bitset extremes.
        const int highest_bid = bid_active_.find_prev_set(n_levels_ - 1);
        const int lowest_ask  = ask_active_.find_next_set(0);
        if (side == Side::BID && best_idx != highest_bid)
            return fail("best_bid_idx inconsistent with bid bitset");
        if (side == Side::ASK && best_idx != lowest_ask)
            return fail("best_ask_idx inconsistent with ask bitset");

        // Walk each live level: intrusive links, qty sums, id map.
        int idx = active.find_next_set(-1);
        while (idx != -1) {
            const PriceLevel& lv = levels[static_cast<std::size_t>(idx)];
            int qty_sum = 0;
            int32_t it  = lv.head_idx;
            int32_t prev = -1;
            int      count = 0;
            while (it != -1) {
                const OrderNode& n = pool_[static_cast<std::size_t>(it)];
                if (n.prev_idx != prev)
                    return fail("broken prev link at node "
                                + std::to_string(it));
                if (n.price_idx != idx)
                    return fail("node price_idx mismatch at node "
                                + std::to_string(it));
                if (n.side != side)
                    return fail("node side mismatch at node "
                                + std::to_string(it));
                if (n.qty <= 0)
                    return fail("non-positive qty on resting node");
                const int32_t* mapped = id_to_pool_idx_.find(n.order_id);
                if (mapped == nullptr || *mapped != it)
                    return fail("id map does not point at node "
                                + std::to_string(it));
                qty_sum += n.qty;
                prev = it;
                it   = n.next_idx;
                ++count;
                if (count > static_cast<int>(pool_.size()))
                    return fail("circular list detected");
            }
            if (lv.tail_idx != prev)
                return fail("tail_idx mismatch at level " + std::to_string(idx));
            if (lv.total_qty != qty_sum)
                return fail("total_qty mismatch at level " + std::to_string(idx));
            live_nodes += count;
            idx = active.find_next_set(idx + 1);
        }
    }

    // Book must not be crossed.
    if (best_bid_idx_ != -1 && best_ask_idx_ != -1 && best_bid_idx_ >= best_ask_idx_)
        return fail("book is crossed");

    // Free list must contain every unused node exactly once.
    {
        std::vector<char> seen(pool_.size(), 0);
        for (int32_t f : free_list_) {
            if (f < 0 || static_cast<std::size_t>(f) >= pool_.size())
                return fail("free-list entry out of range");
            if (seen[static_cast<std::size_t>(f)])
                return fail("duplicate free-list entry");
            seen[static_cast<std::size_t>(f)] = 1;
        }
        std::size_t free_count = 0;
        for (char c : seen) free_count += static_cast<std::size_t>(c);
        if (free_count != pool_.size() - live_nodes)
            return fail("free list size != pool size - live nodes");
        (void)pool_nodes_used;
    }

    if (id_to_pool_idx_.size() != static_cast<std::size_t>(live_nodes))
        return fail("id map size != live nodes");

    return true;
}

std::vector<std::pair<int, int>> OrderBook::dump_levels(Side side) const
{
    std::vector<std::pair<int, int>> out;
    const auto& active = active_for(side);
    const auto& levels = levels_for(side);

    // Bids: best (highest) first, walk down.  Asks: best (lowest) first,
    // walk up.
    int idx = (side == Side::BID) ? best_bid_idx_ : best_ask_idx_;
    while (idx != -1) {
        const PriceLevel& lv = levels[static_cast<std::size_t>(idx)];
        out.emplace_back(idx + price_min_, lv.total_qty);
        idx = (side == Side::BID) ? active.find_prev_set(idx - 1)
                                  : active.find_next_set(idx + 1);
    }
    return out;
}

std::vector<uint64_t> OrderBook::dump_level_orders(Side side, int price) const
{
    std::vector<uint64_t> out;
    const int idx = to_index(price);
    if (idx < 0 || idx >= n_levels_) return out;
    const auto& levels = levels_for(side);
    const PriceLevel& lv = levels[static_cast<std::size_t>(idx)];
    for (int32_t it = lv.head_idx; it != -1; it = pool_[static_cast<std::size_t>(it)].next_idx)
        out.push_back(pool_[static_cast<std::size_t>(it)].order_id);
    return out;
}

} // namespace ob
