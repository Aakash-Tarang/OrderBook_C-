// orderbook_phase4.h

#pragma once
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace p4 {

enum class Side { BID, ASK };
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


class TickBitset {
public:
    explicit TickBitset(int n_bits)
        : n_bits_(n_bits), words_(static_cast<size_t>((n_bits + 63) / 64), 0ULL) {}

    void set(int idx)   { words_[static_cast<size_t>(idx) >> 6] |=  (1ULL << (idx & 63)); }
    void clear(int idx) { words_[static_cast<size_t>(idx) >> 6] &= ~(1ULL << (idx & 63)); }
    bool test(int idx) const {
        return (words_[static_cast<size_t>(idx) >> 6] >> (idx & 63)) & 1ULL;
    }

    
    int find_next_set(int from_idx) const;
    int find_prev_set(int from_idx) const;

private:
    int                    n_bits_;
    std::vector<uint64_t>  words_;
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

    std::vector<OrderNode>                 pool_;
    std::vector<int32_t>                   free_list_;
    std::unordered_map<uint64_t, int32_t>  id_to_pool_idx_;

    int best_bid_idx_ = -1;
    int best_ask_idx_ = -1;

    int to_index(int price) const { return price - price_min_; }

    int32_t alloc_node();
    void    free_node(int32_t idx);

    void                      insert_limit(const Order& order);
    std::vector<TradeReport>  match(Side taker_side);
    void                      remove_level_if_empty(Side side, int idx);
};

} // namespace p4