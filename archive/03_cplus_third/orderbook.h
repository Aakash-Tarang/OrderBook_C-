// orderbook.h
#pragma once
#include <map>
#include <list>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <limits>x
#include <optional>
#include <stdexcept>

enum class Side { BID, ASK };
enum class OrderType { LIMIT, MARKET };

struct Order {
    uint64_t  id;
    Side      side;
    int       price;
    int       qty;
    OrderType type;
};

// A single price level: a doubly-linked list of order IDs
// preserving strict FIFO (time) priority.
struct PriceLevel {
    std::list<uint64_t> orders;   // front = oldest (highest priority)
    int                  total_qty = 0;
};

// Everything the engine needs to cancel an order in O(1):
// the map iterator pointing at the PriceLevel it lives in, and
// the list iterator pointing at the order inside that level.
struct OrderMeta {
    Order order;
    // For bids: iterator into OrderBook::bids_
    // For asks: iterator into OrderBook::asks_
    std::map<int, PriceLevel>::iterator level_it;
    // Iterator into level_it->second.orders
    std::list<uint64_t>::iterator       order_it;
};

struct TradeReport {
    uint64_t bid_id;
    uint64_t ask_id;
    int      price;
    int      qty;
};

class OrderBook {
public:
    // Returns trade reports generated (may be empty for passive orders).
    std::vector<TradeReport> add_order(const Order& order);

    // O(1) true cancellation via stored iterators.
    bool cancel_order(uint64_t order_id);

    // Accessors for benchmarking / display.
    int  best_bid() const;
    int  best_ask() const;
    bool empty_bids() const { return bids_.empty(); }
    bool empty_asks() const { return asks_.empty(); }

private:
    // bids_: sorted ascending by price key; best bid = rbegin() (highest price).
    // asks_: sorted ascending by price key; best ask = begin()  (lowest  price).
    std::map<int, PriceLevel> bids_;
    std::map<int, PriceLevel> asks_;

    // Central registry: order_id -> full metadata (O(1) lookup).
    std::unordered_map<uint64_t, OrderMeta> order_map_;

    // Internal helpers
    void                      insert_limit(const Order& order);
    // taker_side identifies which side just crossed the book, so the trade
    // price can correctly reflect the RESTING (maker) order's price rather
    // than always defaulting to the bid price.
    std::vector<TradeReport>  match(Side taker_side);
    void                      remove_level_if_empty(
                                  Side side,
                                  std::map<int, PriceLevel>::iterator it);
};