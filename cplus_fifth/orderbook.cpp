
#include "orderbook.h"
#include <algorithm>
#include <stdexcept>

namespace p4 {
int TickBitset::find_next_set(int from_idx) const
{
    if (from_idx < 0)       from_idx = 0;
    if (from_idx >= n_bits_) return -1;

    const std::size_t n_base_words = words_.size();

    // ── Phase 1: scan within the starting base word ───────────────────────
    std::size_t word_idx = static_cast<std::size_t>(from_idx) >> 6;
    const int   bit_pos  = from_idx & 63;

    // Zero out bits below from_idx so ctzll finds only bits >= from_idx.
    uint64_t w = words_[word_idx] & (~0ULL << static_cast<unsigned>(bit_pos));
    if (w != 0ULL) {
        int found = static_cast<int>(word_idx * 64ULL)
                    + static_cast<int>(__builtin_ctzll(w));
        return (found < n_bits_) ? found : -1;
    }

    // Phase 1 exhausted. Move to the next base word.
    ++word_idx;
    if (word_idx >= n_base_words) return -1;

    // ── Phase 2: use summary layer to skip empty base words ───────────────
    //
    // Each summary word covers 64 consecutive base words.
    // We want the lowest set summary bit that corresponds to a base word
    // index >= word_idx.
    //
    std::size_t sum_word_idx = word_idx >> 6;          // summary word for word_idx
    const std::size_t n_sum_words = summary_.size();

    // Mask away summary bits for base words we have already visited
    // (those with index < word_idx within this summary word).
    const unsigned sum_bit_pos = static_cast<unsigned>(word_idx & 63ULL);
    uint64_t sw = summary_[sum_word_idx] & (~0ULL << sum_bit_pos);

    while (sw == 0ULL) {
        // This entire summary word is empty → advance to the next one.
        ++sum_word_idx;
        if (sum_word_idx >= n_sum_words) return -1;
        sw = summary_[sum_word_idx];
    }

    // ── Phase 3: jump directly to the non-empty base word ────────────────
    //
    // The lowest set bit in sw tells us which base word is the next live one.
    // base_word_target = sum_word_idx * 64 + ctzll(sw)
    //
    const std::size_t base_word_target =
        (sum_word_idx << 6) + static_cast<std::size_t>(__builtin_ctzll(sw));

    // Guard: base_word_target must be within the allocated base array.
    if (base_word_target >= n_base_words) return -1;

    // words_[base_word_target] is guaranteed non-zero by the summary.
    const int found =
        static_cast<int>(base_word_target * 64ULL)
        + static_cast<int>(__builtin_ctzll(words_[base_word_target]));

    return (found < n_bits_) ? found : -1;
}

// ─────────────────────────────────────────────────────────────────────────────
// find_prev_set  –  highest set bit <= from_idx
//
// Mirror image of find_next_set:
//
//  Phase 1 – Current base word (mask out bits above from_idx) → clzll
//  Phase 2 – Summary layer scan backwards (clzll on summary word)
//  Phase 3 – Direct jump; clzll on base word gives highest set bit
//
// ─────────────────────────────────────────────────────────────────────────────
int TickBitset::find_prev_set(int from_idx) const
{
    if (from_idx >= n_bits_) from_idx = n_bits_ - 1;
    if (from_idx < 0)        return -1;

    // ── Phase 1: scan within the starting base word ───────────────────────
    std::size_t word_idx = static_cast<std::size_t>(from_idx) >> 6;
    const int   bit_pos  = from_idx & 63;

    // Build a mask that keeps only bits [0 .. bit_pos] inclusive.
    // Special-case bit_pos == 63: shifting 1ULL << 64 is UB, so use ~0ULL.
    const uint64_t mask =
        (bit_pos == 63) ? ~0ULL
                        : ((1ULL << static_cast<unsigned>(bit_pos + 1)) - 1ULL);

    uint64_t w = words_[word_idx] & mask;
    if (w != 0ULL) {
        // 63 - clzll gives the position of the highest set bit.
        return static_cast<int>(word_idx * 64ULL)
               + static_cast<int>(63 - __builtin_clzll(w));
    }

    // Phase 1 exhausted. Move to the previous base word.
    if (word_idx == 0) return -1;
    --word_idx;

    // ── Phase 2: use summary layer to skip empty base words ───────────────
    //
    // We want the highest set summary bit whose base-word index <= word_idx.
    //
    std::size_t sum_word_idx = word_idx >> 6;

    // Mask away summary bits for base words above word_idx in this summary word.
    // Bits 0..(word_idx & 63) are the ones we want.
    const unsigned sum_bit_pos = static_cast<unsigned>(word_idx & 63ULL);
    const uint64_t sum_mask =
        (sum_bit_pos == 63) ? ~0ULL
                             : ((1ULL << (sum_bit_pos + 1u)) - 1ULL);

    uint64_t sw = summary_[sum_word_idx] & sum_mask;

    while (sw == 0ULL) {
        // This summary word is entirely empty below word_idx → go back one.
        if (sum_word_idx == 0) return -1;
        --sum_word_idx;
        sw = summary_[sum_word_idx]; // no mask needed: take all bits
    }

    // ── Phase 3: jump directly to the non-empty base word ────────────────
    //
    // Highest set bit in sw = 63 - clzll(sw) → base word index.
    //
    const std::size_t base_word_target =
        (sum_word_idx << 6)
        + static_cast<std::size_t>(63 - __builtin_clzll(sw));

    // words_[base_word_target] is guaranteed non-zero by the summary.
    const int found =
        static_cast<int>(base_word_target * 64ULL)
        + static_cast<int>(63 - __builtin_clzll(words_[base_word_target]));

    return (found < n_bits_) ? found : -1;
}


OrderBook::OrderBook(int price_min, int price_max, size_t pool_capacity)
    : price_min_(price_min),
      price_max_(price_max),
      n_levels_(price_max - price_min + 1),
      bid_levels_(static_cast<std::size_t>(n_levels_)),
      ask_levels_(static_cast<std::size_t>(n_levels_)),
      bid_active_(n_levels_),
      ask_active_(n_levels_),
      pool_(pool_capacity)
{
    free_list_.reserve(pool_capacity);
    for (std::size_t i = pool_capacity; i-- > 0; )
        free_list_.push_back(static_cast<int32_t>(i));
    id_to_pool_idx_.reserve(pool_capacity);
}

int32_t OrderBook::alloc_node()
{
    if (free_list_.empty())
        throw std::runtime_error(
            "OrderBook: order pool exhausted – increase pool_capacity");
    int32_t idx = free_list_.back();
    free_list_.pop_back();
    return idx;
}

void OrderBook::free_node(int32_t idx)
{
    pool_[static_cast<std::size_t>(idx)] = OrderNode{};
    free_list_.push_back(idx);
}

void OrderBook::insert_limit(const Order& order)
{
    int idx = to_index(order.price);
    auto& levels = (order.side == Side::BID) ? bid_levels_ : ask_levels_;
    auto& active = (order.side == Side::BID) ? bid_active_ : ask_active_;
    PriceLevel& level = levels[static_cast<std::size_t>(idx)];

    int32_t node_idx = alloc_node();
    OrderNode& node  = pool_[static_cast<std::size_t>(node_idx)];
    node.order_id  = order.id;
    node.side      = order.side;
    node.price     = order.price;
    node.qty       = order.qty;
    node.price_idx = idx;
    node.next_idx  = -1;
    node.prev_idx  = level.tail_idx;

    if (level.tail_idx != -1)
        pool_[static_cast<std::size_t>(level.tail_idx)].next_idx = node_idx;
    else
        level.head_idx = node_idx;
    level.tail_idx   = node_idx;
    level.total_qty += order.qty;

    active.set(idx);
    id_to_pool_idx_[order.id] = node_idx;

    if (order.side == Side::BID) {
        if (best_bid_idx_ == -1 || idx > best_bid_idx_) best_bid_idx_ = idx;
    } else {
        if (best_ask_idx_ == -1 || idx < best_ask_idx_) best_ask_idx_ = idx;
    }
}

bool OrderBook::cancel_order(uint64_t order_id)
{
    auto it = id_to_pool_idx_.find(order_id);
    if (it == id_to_pool_idx_.end()) return false;

    int32_t    node_idx = it->second;
    OrderNode& node     = pool_[static_cast<std::size_t>(node_idx)];

    Side side = node.side;
    int  idx  = node.price_idx;
    auto& levels = (side == Side::BID) ? bid_levels_ : ask_levels_;
    PriceLevel& level = levels[static_cast<std::size_t>(idx)];

    if (node.prev_idx != -1)
        pool_[static_cast<std::size_t>(node.prev_idx)].next_idx = node.next_idx;
    else
        level.head_idx = node.next_idx;

    if (node.next_idx != -1)
        pool_[static_cast<std::size_t>(node.next_idx)].prev_idx = node.prev_idx;
    else
        level.tail_idx = node.prev_idx;

    level.total_qty -= node.qty;

    id_to_pool_idx_.erase(it);
    free_node(node_idx);

    remove_level_if_empty(side, idx);
    return true;
}

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
    auto& levels = (side == Side::BID) ? bid_levels_ : ask_levels_;
    auto& active = (side == Side::BID) ? bid_active_ : ask_active_;

    if (levels[static_cast<std::size_t>(idx)].head_idx != -1)
        return; // level still has orders; nothing to do

    active.clear(idx); // updates both Layer 0 and Layer 1

    if (side == Side::BID && idx == best_bid_idx_)
        best_bid_idx_ = bid_active_.find_prev_set(idx - 1);
    if (side == Side::ASK && idx == best_ask_idx_)
        best_ask_idx_ = ask_active_.find_next_set(idx + 1);
}

std::vector<TradeReport> OrderBook::match(Side taker_side)
{
    std::vector<TradeReport> reports;

    while (best_bid_idx_ != -1 && best_ask_idx_ != -1 &&
           best_bid_idx_ >= best_ask_idx_)
    {
        PriceLevel& bid_level = bid_levels_[static_cast<std::size_t>(best_bid_idx_)];
        PriceLevel& ask_level = ask_levels_[static_cast<std::size_t>(best_ask_idx_)];

        while (bid_level.head_idx != -1 && ask_level.head_idx != -1) {
            int32_t    bid_node_idx = bid_level.head_idx;
            int32_t    ask_node_idx = ask_level.head_idx;
            OrderNode& bid_node     = pool_[static_cast<std::size_t>(bid_node_idx)];
            OrderNode& ask_node     = pool_[static_cast<std::size_t>(ask_node_idx)];

            int trade_price = (taker_side == Side::BID) ? ask_node.price
                                                         : bid_node.price;
            int traded_qty  = std::min(bid_node.qty, ask_node.qty);

            bid_node.qty        -= traded_qty;
            ask_node.qty        -= traded_qty;
            bid_level.total_qty -= traded_qty;
            ask_level.total_qty -= traded_qty;

            reports.push_back({bid_node.order_id, ask_node.order_id,
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

    return reports;
}

std::vector<TradeReport> OrderBook::add_order(const Order& order)
{
    if (order.qty <= 0)
        throw std::invalid_argument("Order quantity must be positive");

    int idx = to_index(order.price);
    if (idx < 0 || idx >= n_levels_)
        throw std::out_of_range(
            "OrderBook: order price outside configured tick range");

    if (order.type == OrderType::LIMIT) {
        insert_limit(order);
        return match(order.side);
    }

    // Market order path (unchanged).
    std::vector<TradeReport> reports;
    int remaining = order.qty;

    if (order.side == Side::BID) {
        while (remaining > 0 && best_ask_idx_ != -1) {
            PriceLevel& level = ask_levels_[static_cast<std::size_t>(best_ask_idx_)];
            while (remaining > 0 && level.head_idx != -1) {
                int32_t    node_idx = level.head_idx;
                OrderNode& node     = pool_[static_cast<std::size_t>(node_idx)];

                int traded      = std::min(remaining, node.qty);
                remaining          -= traded;
                node.qty           -= traded;
                level.total_qty    -= traded;

                reports.push_back({order.id, node.order_id, node.price, traded});

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
            remove_level_if_empty(Side::ASK, best_ask_idx_);
        }
    } else {
        while (remaining > 0 && best_bid_idx_ != -1) {
            PriceLevel& level = bid_levels_[static_cast<std::size_t>(best_bid_idx_)];
            while (remaining > 0 && level.head_idx != -1) {
                int32_t    node_idx = level.head_idx;
                OrderNode& node     = pool_[static_cast<std::size_t>(node_idx)];

                int traded      = std::min(remaining, node.qty);
                remaining          -= traded;
                node.qty           -= traded;
                level.total_qty    -= traded;

                reports.push_back({node.order_id, order.id, node.price, traded});

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
            remove_level_if_empty(Side::BID, best_bid_idx_);
        }
    }
    return reports;
}

} // namespace p4