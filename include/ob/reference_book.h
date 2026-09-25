#pragma once
/*
 * reference_book.h – Deliberately naive reference limit order book.
 *
 * Purpose
 * ───────
 * This is the correctness oracle for the optimised engine
 * (ob::OrderBook).  It uses plain std::map + std::deque + linear scans and
 * has no performance ambition whatsoever: every line is meant to be
 * *obviously* correct.  The differential fuzz tests (tests/) drive both
 * books with identical random op streams and demand byte-identical trade
 * output and identical book state after every operation.
 *
 * Semantics mirrored from ob::OrderBook (they MUST stay in lockstep):
 *   - price-time priority, FIFO within a level
 *   - trade price = resting order's price (taker pays the spread)
 *   - market orders sweep best-first, unfilled remainder is dropped
 *   - order id 0 rejected; empty book: best_bid == INT_MIN,
 *     best_ask == INT_MAX
 *   - duplicate live order ids are rejected
 */

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

#include "orderbook.h"   // ob::Order, ob::Side, ob::OrderType, ob::TradeReport

namespace ob {

class ReferenceBook {
public:
    // Adds the order and returns true when accepted.  Trades are appended
    // to `trades_out` in execution order (not cleared first).
    bool add_order(const Order& order, std::vector<TradeReport>& trades_out)
    {
        if (order.id == 0)  return false;
        if (order.qty <= 0) return false;
        if (live_.count(order.id) != 0) return false;

        if (order.type == OrderType::LIMIT) {
            rest(order);
            match(order.side, trades_out);
            return true;
        }

        // ── MARKET: sweep the opposite side best-first, FIFO within level ──
        int remaining = order.qty;
        while (remaining > 0) {
            if (order.side == Side::BID) {
                if (asks_.empty()) break;
                RefLevel& lvl = asks_.begin()->second;
                const int price = asks_.begin()->first;

                ROrder& front = lvl.q.front();
                const int traded = std::min(remaining, front.qty);
                remaining -= traded;
                front.qty -= traded;
                lvl.total -= traded;
                trades_out.push_back({order.id, front.id, price, traded});

                if (front.qty == 0) {
                    live_.erase(front.id);
                    lvl.q.pop_front();
                    if (lvl.q.empty()) asks_.erase(asks_.begin());
                }
            } else {
                if (bids_.empty()) break;
                RefLevel& lvl = bids_.begin()->second;
                const int price = bids_.begin()->first;

                ROrder& front = lvl.q.front();
                const int traded = std::min(remaining, front.qty);
                remaining -= traded;
                front.qty -= traded;
                lvl.total -= traded;
                trades_out.push_back({front.id, order.id, price, traded});

                if (front.qty == 0) {
                    live_.erase(front.id);
                    lvl.q.pop_front();
                    if (lvl.q.empty()) bids_.erase(bids_.begin());
                }
            }
        }
        return true;
    }

    bool cancel_order(uint64_t order_id)
    {
        auto it = live_.find(order_id);
        if (it == live_.end()) return false;

        const Side side  = it->second.first;
        const int  price = it->second.second;

        if (side == Side::BID) {
            auto lvl = bids_.find(price);
            assert(lvl != bids_.end());
            erase_from_level(lvl->second, order_id);
            if (lvl->second.q.empty()) bids_.erase(lvl);
        } else {
            auto lvl = asks_.find(price);
            assert(lvl != asks_.end());
            erase_from_level(lvl->second, order_id);
            if (lvl->second.q.empty()) asks_.erase(lvl);
        }
        live_.erase(it);
        return true;
    }

    int best_bid() const
    {
        return bids_.empty() ? std::numeric_limits<int>::min()
                             : bids_.begin()->first;
    }

    int best_ask() const
    {
        return asks_.empty() ? std::numeric_limits<int>::max()
                             : asks_.begin()->first;
    }

    std::size_t live_orders() const { return live_.size(); }

    // (price, total_qty) best price first – mirrors OrderBook::dump_levels.
    std::vector<std::pair<int, int>> dump_levels(Side side) const
    {
        std::vector<std::pair<int, int>> out;
        if (side == Side::BID)
            for (const auto& kv : bids_) out.emplace_back(kv.first, kv.second.total);
        else
            for (const auto& kv : asks_) out.emplace_back(kv.first, kv.second.total);
        return out;
    }

    // FIFO ids at one level – mirrors OrderBook::dump_level_orders.
    std::vector<uint64_t> dump_level_orders(Side side, int price) const
    {
        std::vector<uint64_t> out;
        if (side == Side::BID) {
            auto lvl = bids_.find(price);
            if (lvl == bids_.end()) return out;
            for (const auto& o : lvl->second.q) out.push_back(o.id);
        } else {
            auto lvl = asks_.find(price);
            if (lvl == asks_.end()) return out;
            for (const auto& o : lvl->second.q) out.push_back(o.id);
        }
        return out;
    }

private:
    struct ROrder { uint64_t id; int qty; };
    struct RefLevel { std::deque<ROrder> q; int total = 0; };

    using BidMap = std::map<int, RefLevel, std::greater<int>>;
    using AskMap = std::map<int, RefLevel, std::less<int>>;

    BidMap bids_;   // best (highest) first
    AskMap asks_;   // best (lowest) first

    std::unordered_map<uint64_t, std::pair<Side, int>> live_; // id → (side, price)

    void rest(const Order& order)
    {
        if (order.side == Side::BID) {
            RefLevel& lvl = bids_[order.price];
            lvl.q.push_back({order.id, order.qty});
            lvl.total += order.qty;
        } else {
            RefLevel& lvl = asks_[order.price];
            lvl.q.push_back({order.id, order.qty});
            lvl.total += order.qty;
        }
        live_.emplace(order.id, std::make_pair(order.side, order.price));
    }

    // Trades while the book is crossed.
    //
    // Because the resting book is never allowed to be crossed at rest, the
    // crossing pair always involves the order just inserted, so this loop
    // produces exactly the fills an explicit taker-driven loop would.
    // The trade prints at the resting price: ask best when a buy rests
    // against it, bid best when a sell rests against it.
    void match(Side taker_side, std::vector<TradeReport>& out)
    {
        while (!bids_.empty() && !asks_.empty() &&
               bids_.begin()->first >= asks_.begin()->first)
        {
            RefLevel& bid_lvl = bids_.begin()->second;
            RefLevel& ask_lvl = asks_.begin()->second;

            const int trade_price = (taker_side == Side::BID)
                                        ? asks_.begin()->first
                                        : bids_.begin()->first;

            while (!bid_lvl.q.empty() && !ask_lvl.q.empty()) {
                ROrder& bid = bid_lvl.q.front();
                ROrder& ask = ask_lvl.q.front();

                const int traded = std::min(bid.qty, ask.qty);
                bid.qty      -= traded;
                ask.qty      -= traded;
                bid_lvl.total -= traded;
                ask_lvl.total -= traded;

                out.push_back({bid.id, ask.id, trade_price, traded});

                if (bid.qty == 0) {
                    live_.erase(bid.id);
                    bid_lvl.q.pop_front();
                }
                if (ask.qty == 0) {
                    live_.erase(ask.id);
                    ask_lvl.q.pop_front();
                }
            }
            if (bid_lvl.q.empty()) bids_.erase(bids_.begin());
            if (ask_lvl.q.empty()) asks_.erase(asks_.begin());
        }
    }

    void erase_from_level(RefLevel& lvl, uint64_t id)
    {
        for (auto it = lvl.q.begin(); it != lvl.q.end(); ++it) {
            if (it->id == id) {
                lvl.total -= it->qty;
                lvl.q.erase(it);
                return;
            }
        }
        assert(false && "reference book: cancel of id missing from level");
    }
};

} // namespace ob
