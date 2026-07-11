// orderbook_phase4.cpp
#include "orderbook.h"
#include <algorithm>
#include <stdexcept>

namespace p4 {

int TickBitset::find_next_set(int from_idx) const
{
    if (from_idx < 0) from_idx = 0;
    if (from_idx >= n_bits_) return -1;

    size_t word_idx = static_cast<size_t>(from_idx) >> 6;
    int    bit_idx  = from_idx & 63;

    uint64_t w = words_[word_idx] & (~0ULL << bit_idx);
    if (w != 0) {
        int found = static_cast<int>(word_idx * 64) + __builtin_ctzll(w);
        return found < n_bits_ ? found : -1;
    }
    for (size_t i = word_idx + 1; i < words_.size(); ++i) {
        if (words_[i] != 0) {
            int found = static_cast<int>(i * 64) + __builtin_ctzll(words_[i]);
            return found < n_bits_ ? found : -1;
        }
    }
    return -1;
}

int TickBitset::find_prev_set(int from_idx) const
{
    if (from_idx >= n_bits_) from_idx = n_bits_ - 1;
    if (from_idx < 0) return -1;

    size_t word_idx = static_cast<size_t>(from_idx) >> 6;
    int    bit_idx  = from_idx & 63;

    
    uint64_t mask = (bit_idx == 63) ? ~0ULL : ((1ULL << (bit_idx + 1)) - 1);
    uint64_t w = words_[word_idx] & mask;
    if (w != 0)
        return static_cast<int>(word_idx * 64) + (63 - __builtin_clzll(w));

    for (int i = static_cast<int>(word_idx) - 1; i >= 0; --i) {
        if (words_[static_cast<size_t>(i)] != 0)
            return i * 64 + (63 - __builtin_clzll(words_[static_cast<size_t>(i)]));
    }
    return -1;
}

OrderBook::OrderBook(int price_min, int price_max, size_t pool_capacity)
    : price_min_(price_min),
      price_max_(price_max),
      n_levels_(price_max - price_min + 1),
      bid_levels_(static_cast<size_t>(n_levels_)),
      ask_levels_(static_cast<size_t>(n_levels_)),
      bid_active_(n_levels_),
      ask_active_(n_levels_),
      pool_(pool_capacity)
{
    free_list_.reserve(pool_capacity);
    for (size_t i = pool_capacity; i-- > 0; )
        free_list_.push_back(static_cast<int32_t>(i));
    id_to_pool_idx_.reserve(pool_capacity);
}

int32_t OrderBook::alloc_node()
{
    if (free_list_.empty())
        throw std::runtime_error(
            "OrderBook: order pool exhausted - increase pool_capacity");
    int32_t idx = free_list_.back();
    free_list_.pop_back();
    return idx;
}

void OrderBook::free_node(int32_t idx)
{
    pool_[static_cast<size_t>(idx)] = OrderNode{};  // reset, avoid stale links
    free_list_.push_back(idx);
}

void OrderBook::insert_limit(const Order& order)
{
    int idx = to_index(order.price);
    auto& levels = (order.side == Side::BID) ? bid_levels_  : ask_levels_;
    auto& active = (order.side == Side::BID) ? bid_active_  : ask_active_;
    PriceLevel& level = levels[static_cast<size_t>(idx)];

    int32_t node_idx = alloc_node();
    OrderNode& node = pool_[static_cast<size_t>(node_idx)];
    node.order_id  = order.id;
    node.side      = order.side;
    node.price     = order.price;
    node.qty       = order.qty;
    node.price_idx = idx;
    node.next_idx  = -1;
    node.prev_idx  = level.tail_idx;

    if (level.tail_idx != -1)
        pool_[static_cast<size_t>(level.tail_idx)].next_idx = node_idx;
    else
        level.head_idx = node_idx;
    level.tail_idx = node_idx;
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
    if (it == id_to_pool_idx_.end())
        return false;   // already filled or never existed

    int32_t node_idx = it->second;
    OrderNode& node  = pool_[static_cast<size_t>(node_idx)];

    Side side = node.side;
    int  idx  = node.price_idx;
    auto& levels = (side == Side::BID) ? bid_levels_ : ask_levels_;
    PriceLevel& level = levels[static_cast<size_t>(idx)];

    // O(1) unlink from the intrusive doubly-linked list.
    if (node.prev_idx != -1)
        pool_[static_cast<size_t>(node.prev_idx)].next_idx = node.next_idx;
    else
        level.head_idx = node.next_idx;

    if (node.next_idx != -1)
        pool_[static_cast<size_t>(node.next_idx)].prev_idx = node.prev_idx;
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

    if (levels[static_cast<size_t>(idx)].head_idx != -1)
        return;   // still occupied, nothing to do

    active.clear(idx);


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
        PriceLevel& bid_level = bid_levels_[static_cast<size_t>(best_bid_idx_)];
        PriceLevel& ask_level = ask_levels_[static_cast<size_t>(best_ask_idx_)];

        while (bid_level.head_idx != -1 && ask_level.head_idx != -1) {
            int32_t bid_node_idx = bid_level.head_idx;
            int32_t ask_node_idx = ask_level.head_idx;
            OrderNode& bid_node = pool_[static_cast<size_t>(bid_node_idx)];
            OrderNode& ask_node = pool_[static_cast<size_t>(ask_node_idx)];

            // Trade prices at the resting (maker) side, per taker_side.
            int trade_price = (taker_side == Side::BID) ? ask_node.price
                                                          : bid_node.price;
            int traded_qty  = std::min(bid_node.qty, ask_node.qty);

            bid_node.qty          -= traded_qty;
            ask_node.qty          -= traded_qty;
            bid_level.total_qty   -= traded_qty;
            ask_level.total_qty   -= traded_qty;

            reports.push_back({bid_node.order_id, ask_node.order_id,
                                trade_price, traded_qty});

            if (bid_node.qty == 0) {
                bid_level.head_idx = bid_node.next_idx;
                if (bid_level.head_idx != -1)
                    pool_[static_cast<size_t>(bid_level.head_idx)].prev_idx = -1;
                else
                    bid_level.tail_idx = -1;
                id_to_pool_idx_.erase(bid_node.order_id);
                free_node(bid_node_idx);
            }
            if (ask_node.qty == 0) {
                ask_level.head_idx = ask_node.next_idx;
                if (ask_level.head_idx != -1)
                    pool_[static_cast<size_t>(ask_level.head_idx)].prev_idx = -1;
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

    // Market order: executes immediately, never rests in the book.
    std::vector<TradeReport> reports;
    int remaining = order.qty;

    if (order.side == Side::BID) {
        while (remaining > 0 && best_ask_idx_ != -1) {
            PriceLevel& level = ask_levels_[static_cast<size_t>(best_ask_idx_)];
            while (remaining > 0 && level.head_idx != -1) {
                int32_t node_idx = level.head_idx;
                OrderNode& node = pool_[static_cast<size_t>(node_idx)];

                int traded = std::min(remaining, node.qty);
                remaining          -= traded;
                node.qty           -= traded;
                level.total_qty    -= traded;

                reports.push_back({order.id, node.order_id, node.price, traded});

                if (node.qty == 0) {
                    level.head_idx = node.next_idx;
                    if (level.head_idx != -1)
                        pool_[static_cast<size_t>(level.head_idx)].prev_idx = -1;
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
            PriceLevel& level = bid_levels_[static_cast<size_t>(best_bid_idx_)];
            while (remaining > 0 && level.head_idx != -1) {
                int32_t node_idx = level.head_idx;
                OrderNode& node = pool_[static_cast<size_t>(node_idx)];

                int traded = std::min(remaining, node.qty);
                remaining          -= traded;
                node.qty           -= traded;
                level.total_qty    -= traded;

                reports.push_back({node.order_id, order.id, node.price, traded});

                if (node.qty == 0) {
                    level.head_idx = node.next_idx;
                    if (level.head_idx != -1)
                        pool_[static_cast<size_t>(level.head_idx)].prev_idx = -1;
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