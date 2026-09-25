// orderbook.cpp
#include "orderbook.h"
#include <algorithm>
#include <stdexcept>

std::vector<TradeReport> OrderBook::add_order(const Order& order)
{
    if (order.qty <= 0)
        throw std::invalid_argument("Order quantity must be positive");

    if (order.type == OrderType::LIMIT) {
        insert_limit(order);
        return match(order.side);
    }

    // Market order: execute immediately against the opposite side.
    std::vector<TradeReport> reports;
    int remaining = order.qty;

    if (order.side == Side::BID) {
        while (remaining > 0 && !asks_.empty()) {
            auto ask_level_it = asks_.begin();        
            PriceLevel& level = ask_level_it->second;

            while (remaining > 0 && !level.orders.empty()) {
                uint64_t ask_id = level.orders.front();
                OrderMeta& meta = order_map_.at(ask_id);
                Order&     ask  = meta.order;

                int traded = std::min(remaining, ask.qty);
                remaining        -= traded;
                ask.qty          -= traded;
                level.total_qty  -= traded;

                reports.push_back({order.id, ask_id,
                                   ask_level_it->first, traded});

                if (ask.qty == 0) {
                    level.orders.pop_front();
                    order_map_.erase(ask_id);
                }
            }
            remove_level_if_empty(Side::ASK, ask_level_it);
        }
    } else {
        while (remaining > 0 && !bids_.empty()) {
            auto bid_level_it = std::prev(bids_.end()); 
            PriceLevel& level = bid_level_it->second;

            while (remaining > 0 && !level.orders.empty()) {
                uint64_t bid_id = level.orders.front();
                OrderMeta& meta = order_map_.at(bid_id);
                Order&     bid  = meta.order;

                int traded = std::min(remaining, bid.qty);
                remaining        -= traded;
                bid.qty          -= traded;
                level.total_qty  -= traded;

                reports.push_back({bid_id, order.id,
                                   bid_level_it->first, traded});

                if (bid.qty == 0) {
                    level.orders.pop_front();
                    order_map_.erase(bid_id);
                }
            }
            remove_level_if_empty(Side::BID, bid_level_it);
        }
    }
    return reports;
}

bool OrderBook::cancel_order(uint64_t order_id)
{
    auto it = order_map_.find(order_id);
    if (it == order_map_.end())
        return false;   // already filled or never existed

    OrderMeta& meta   = it->second;
    PriceLevel& level = meta.level_it->second;

    // O(1): erase directly via stored list iterator.
    level.total_qty -= meta.order.qty;
    level.orders.erase(meta.order_it);

    // Clean up the price level from the map if it is now empty.
    if (meta.order.side == Side::BID)
        remove_level_if_empty(Side::BID, meta.level_it);
    else
        remove_level_if_empty(Side::ASK, meta.level_it);

    order_map_.erase(it);
    return true;
}

int OrderBook::best_bid() const
{
    if (bids_.empty())
        return std::numeric_limits<int>::min();
    return bids_.rbegin()->first;
}

int OrderBook::best_ask() const
{
    if (asks_.empty())
        return std::numeric_limits<int>::max();
    return asks_.begin()->first;
}


void OrderBook::insert_limit(const Order& order)
{
    auto& book = (order.side == Side::BID) ? bids_ : asks_;

    // emplace default-constructs PriceLevel if price not present yet.
    auto level_it = book.emplace(order.price, PriceLevel{}).first;
    PriceLevel& level = level_it->second;

    level.total_qty += order.qty;
    level.orders.push_back(order.id);          // FIFO: new orders go to back

    // Store iterator pair for O(1) future cancellation.
    OrderMeta meta;
    meta.order    = order;
    meta.level_it = level_it;
    meta.order_it = std::prev(level.orders.end());

    order_map_.emplace(order.id, std::move(meta));
}

std::vector<TradeReport> OrderBook::match(Side taker_side)
{
    std::vector<TradeReport> reports;

    while (!bids_.empty() && !asks_.empty()) {
        auto bid_level_it = std::prev(bids_.end()); // highest bid: O(1)
        auto ask_level_it = asks_.begin();           // lowest  ask: O(1)

        if (bid_level_it->first < ask_level_it->first)
            break;   // spread is positive — no cross

        PriceLevel& bid_level = bid_level_it->second;
        PriceLevel& ask_level = ask_level_it->second;

        // Both levels are guaranteed non-empty: they are removed from the
        // map the moment their last order is matched or cancelled, so no
        // "zombie" order skipping is ever required here.
        while (!bid_level.orders.empty() &&
               !ask_level.orders.empty())
        {
            uint64_t bid_id = bid_level.orders.front();
            uint64_t ask_id = ask_level.orders.front();

            OrderMeta& bid_meta = order_map_.at(bid_id);
            OrderMeta& ask_meta = order_map_.at(ask_id);

            Order& bid = bid_meta.order;
            Order& ask = ask_meta.order;

            // Trade executes at the RESTING (maker) order's price.
            // The side that just triggered this match() call (taker_side)
            // is the aggressor; the other side is the maker and sets price.
            int trade_price = (taker_side == Side::BID)
                                   ? ask_level_it->first   // maker = ask
                                   : bid_level_it->first;  // maker = bid

            int traded_qty  = std::min(bid.qty, ask.qty);

            bid.qty              -= traded_qty;
            ask.qty              -= traded_qty;
            bid_level.total_qty  -= traded_qty;
            ask_level.total_qty  -= traded_qty;

            reports.push_back({bid_id, ask_id, trade_price, traded_qty});

            if (bid.qty == 0) {
                bid_level.orders.pop_front();
                order_map_.erase(bid_id);
            }
            if (ask.qty == 0) {
                ask_level.orders.pop_front();
                order_map_.erase(ask_id);
            }
        }

        // Remove exhausted price levels immediately —
        // the next best price is then the new begin()/rbegin(), O(1).
        remove_level_if_empty(Side::BID, bid_level_it);
        remove_level_if_empty(Side::ASK, ask_level_it);
    }

    return reports;
}

void OrderBook::remove_level_if_empty(
        Side side,
        std::map<int, PriceLevel>::iterator it)
{
    if (it->second.orders.empty()) {
        if (side == Side::BID)
            bids_.erase(it);
        else
            asks_.erase(it);
    }
}