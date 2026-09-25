// OrderBook unit tests – edge cases, invariants, and documented semantics.
#include "test_framework.h"

#include <string>
#include <vector>

#include "ob/orderbook.h"

namespace {

std::string invariant_err;

bool ok(const ob::OrderBook& b) { return b.verify_invariants(invariant_err); }

ob::Order limit(uint64_t id, ob::Side s, int px, int qty)
{
    return ob::Order{id, s, px, qty, ob::OrderType::LIMIT};
}
ob::Order market(uint64_t id, ob::Side s, int qty)
{
    return ob::Order{id, s, 0, qty, ob::OrderType::MARKET};
}

} // namespace

OB_TEST(book_accepts_and_rests_basic_limit)
{
    ob::OrderBook b(1, 20000, 64);
    std::vector<ob::TradeReport> trades;

    OB_CHECK(b.add_order(limit(1, ob::Side::BID, 9999, 10), trades));
    OB_CHECK(trades.empty());
    OB_CHECK(b.best_bid() == 9999);
    OB_CHECK(b.best_ask() == INT32_MAX);
    OB_CHECK(ok(b));

    OB_CHECK(b.add_order(limit(2, ob::Side::ASK, 10001, 10), trades));
    OB_CHECK(trades.empty());
    OB_CHECK(b.best_ask() == 10001);
    OB_CHECK(b.best_bid() == 9999);  // no match, spread preserved
    OB_CHECK(ok(b));
}

OB_TEST(book_matches_crossing_limit_and_prints_resting_price)
{
    ob::OrderBook b(1, 20000, 64);
    std::vector<ob::TradeReport> trades;

    b.add_order(limit(1, ob::Side::BID, 9999, 10), trades);
    b.add_order(limit(2, ob::Side::ASK, 10000, 10), trades);
    OB_CHECK(trades.empty());

    // Buy taker crosses the resting ask → prints at the ask price.
    b.add_order(limit(3, ob::Side::BID, 10002, 5), trades);
    OB_REQUIRE(trades.size() == 1);
    OB_CHECK(trades[0].bid_id == 3);
    OB_CHECK(trades[0].ask_id == 2);
    OB_CHECK(trades[0].price == 10000);
    OB_CHECK(trades[0].qty == 5);
    OB_CHECK(b.best_ask() == 10000);   // half the resting ask remains
    OB_CHECK(b.best_bid() == 9999);    // the taker filled fully and is gone
    OB_CHECK(ok(b));

    // Sell taker (15 @ 9998) sweeps the resting bid, prints at the resting
    // bid price, and rests the unfilled remainder as the new best ask.
    trades.clear();
    b.add_order(limit(4, ob::Side::ASK, 9998, 15), trades);
    OB_REQUIRE(trades.size() == 1);
    OB_CHECK(trades[0].bid_id == 1);
    OB_CHECK(trades[0].ask_id == 4);
    OB_CHECK(trades[0].price == 9999);
    OB_CHECK(trades[0].qty == 10);
    OB_CHECK(b.empty_bids());
    OB_CHECK(b.best_ask() == 9998);    // remainder now best ask
    OB_CHECK(ok(b));
}

OB_TEST(book_fifo_within_level)
{
    ob::OrderBook b(1, 20000, 64);
    std::vector<ob::TradeReport> trades;

    b.add_order(limit(10, ob::Side::BID, 100, 10), trades);
    b.add_order(limit(11, ob::Side::BID, 100, 20), trades);
    b.add_order(limit(12, ob::Side::BID, 100, 30), trades);

    const auto ids = b.dump_level_orders(ob::Side::BID, 100);
    OB_REQUIRE(ids.size() == 3);
    OB_CHECK(ids[0] == 10 && ids[1] == 11 && ids[2] == 12);

    // A sell sweeps strictly FIFO: 25 filled = all of id 10 (10) + 15 of
    // id 11 (20).  Orders 11 (partial) and 12 remain, in time order.
    b.add_order(market(99, ob::Side::ASK, 25), trades);
    const auto after = b.dump_level_orders(ob::Side::BID, 100);
    OB_REQUIRE(after.size() == 2);
    OB_CHECK(after[0] == 11);
    OB_CHECK(after[1] == 12);
    OB_CHECK(ok(b));
}

OB_TEST(book_partial_fill_keeps_time_priority)
{
    ob::OrderBook b(1, 20000, 64);
    std::vector<ob::TradeReport> trades;

    b.add_order(limit(1, ob::Side::BID, 100, 50), trades);
    b.add_order(limit(2, ob::Side::BID, 100, 50), trades);
    b.add_order(market(9, ob::Side::ASK, 30), trades); // first order → 20 left

    const auto ids = b.dump_level_orders(ob::Side::BID, 100);
    OB_REQUIRE(ids.size() == 2);
    OB_CHECK(ids[0] == 1); // still front, partially filled
    OB_CHECK(ids[1] == 2);
    OB_CHECK(ok(b));
}

OB_TEST(book_cancel_removes_and_restores_best)
{
    ob::OrderBook b(1, 20000, 64);
    std::vector<ob::TradeReport> trades;

    b.add_order(limit(1, ob::Side::BID, 100, 10), trades);
    b.add_order(limit(2, ob::Side::BID, 101, 10), trades);
    b.add_order(limit(3, ob::Side::ASK, 105, 10), trades);

    OB_CHECK(b.cancel_order(2));
    OB_CHECK(b.best_bid() == 100);
    OB_CHECK(!b.cancel_order(2));  // already gone
    OB_CHECK(b.cancel_order(1));
    OB_CHECK(b.empty_bids());
    OB_CHECK(ok(b));

    b.cancel_order(3);
    OB_CHECK(b.empty_asks());
    OB_CHECK(b.best_bid() == INT32_MIN);
    OB_CHECK(b.best_ask() == INT32_MAX);
    OB_CHECK(ok(b));
}

OB_TEST(book_self_cross_protection_rejects_duplicate_id)
{
    ob::OrderBook b(1, 20000, 64);
    std::vector<ob::TradeReport> trades;

    OB_CHECK(b.add_order(limit(5, ob::Side::BID, 100, 10), trades));
    OB_CHECK(!b.add_order(limit(5, ob::Side::BID, 101, 10), trades)); // dup id
    OB_CHECK(!b.add_order(limit(0, ob::Side::BID, 101, 10), trades)); // id 0
    OB_CHECK(b.live_orders() == 1);
    OB_CHECK(ok(b));
}

OB_TEST(book_rejects_bad_input)
{
    ob::OrderBook b(1, 20000, 64);
    std::vector<ob::TradeReport> trades;

    OB_CHECK(!b.add_order(limit(1, ob::Side::BID, 100, 0), trades));   // qty 0
    OB_CHECK(!b.add_order(limit(2, ob::Side::BID, 100, -5), trades));  // neg qty
    OB_CHECK(!b.add_order(limit(3, ob::Side::BID, 0, 10), trades));    // below range
    OB_CHECK(!b.add_order(limit(4, ob::Side::BID, 20001, 10), trades));// above range
    OB_CHECK(b.live_orders() == 0);
    OB_CHECK(ok(b));
}

OB_TEST(book_market_order_larger_than_book_drops_remainder)
{
    ob::OrderBook b(1, 20000, 64);
    std::vector<ob::TradeReport> trades;

    b.add_order(limit(1, ob::Side::ASK, 100, 10), trades);
    b.add_order(limit(2, ob::Side::ASK, 101, 10), trades);

    b.add_order(market(9, ob::Side::BID, 100), trades);
    OB_REQUIRE(trades.size() == 2);
    int filled = 0;
    for (const auto& t : trades) filled += t.qty;
    OB_CHECK(filled == 20);            // only what the book had
    OB_CHECK(b.empty_asks());
    OB_CHECK(ok(b));
}

OB_TEST(book_market_order_on_empty_book_is_noop)
{
    ob::OrderBook b(1, 20000, 64);
    std::vector<ob::TradeReport> trades;
    b.add_order(market(1, ob::Side::BID, 50), trades);
    OB_CHECK(trades.empty());
    OB_CHECK(b.live_orders() == 0);
    OB_CHECK(ok(b));
}

OB_TEST(book_replace_loses_priority_and_can_cross)
{
    ob::OrderBook b(1, 20000, 64);
    std::vector<ob::TradeReport> trades;

    b.add_order(limit(1, ob::Side::BID, 100, 10), trades);
    b.add_order(limit(2, ob::Side::BID, 100, 10), trades);
    OB_REQUIRE(b.dump_level_orders(ob::Side::BID, 100).size() == 2);

    // Amend order 1 to a worse price: goes to the BACK of the new level.
    trades.clear();
    OB_CHECK(b.replace_order(1, 99, 10, trades));
    OB_CHECK(trades.empty());
    const auto lvl100 = b.dump_level_orders(ob::Side::BID, 100);
    const auto lvl99  = b.dump_level_orders(ob::Side::BID, 99);
    OB_REQUIRE(lvl100.size() == 1);
    OB_CHECK(lvl100[0] == 2);
    OB_REQUIRE(lvl99.size() == 1);
    OB_CHECK(lvl99[0] == 1);
    OB_CHECK(ok(b));

    // Amend order 2 upward (same side – replace keeps the side): it rests
    // as the new best bid.
    trades.clear();
    OB_CHECK(b.replace_order(2, 105, 10, trades));
    OB_CHECK(trades.empty());
    const auto lvl105 = b.dump_level_orders(ob::Side::BID, 105);
    OB_REQUIRE(lvl105.size() == 1);
    OB_CHECK(lvl105[0] == 2);
    OB_CHECK(b.best_bid() == 105);
    OB_CHECK(b.empty_asks());
    OB_CHECK(ok(b));

    // A resting ask appears; amending order 1 across it executes at the
    // resting ask price, leaving the taker remainder in the book.
    trades.clear();
    OB_CHECK(b.add_order(limit(3, ob::Side::ASK, 106, 5), trades));
    OB_CHECK(trades.empty());

    trades.clear();
    OB_CHECK(b.replace_order(1, 108, 8, trades));
    OB_REQUIRE(trades.size() == 1);
    OB_CHECK(trades[0].bid_id == 1);
    OB_CHECK(trades[0].ask_id == 3);
    OB_CHECK(trades[0].qty == 5);       // limited by the resting ask
    OB_CHECK(trades[0].price == 106);   // resting ask price
    OB_CHECK(b.best_bid() == 108);      // remainder (3) rests at 108
    OB_CHECK(b.empty_asks());
    OB_CHECK(ok(b));
}

OB_TEST(book_replace_rejects_bad_input)
{
    ob::OrderBook b(1, 20000, 64);
    std::vector<ob::TradeReport> trades;
    b.add_order(limit(1, ob::Side::BID, 100, 10), trades);

    OB_CHECK(!b.replace_order(999, 101, 5, trades));   // unknown id
    OB_CHECK(!b.replace_order(1, 101, 0, trades));     // bad qty
    OB_CHECK(!b.replace_order(1, 99999, 5, trades));   // out of range
    OB_CHECK(b.live_orders() == 1);                    // untouched
    OB_CHECK(ok(b));
}

OB_TEST(book_pool_exhaustion_rejects_gracefully)
{
    ob::OrderBook b(1, 20000, 3);
    std::vector<ob::TradeReport> trades;

    OB_CHECK(b.add_order(limit(1, ob::Side::BID, 100, 5), trades));
    OB_CHECK(b.add_order(limit(2, ob::Side::BID, 101, 5), trades));
    OB_CHECK(b.add_order(limit(3, ob::Side::BID, 102, 5), trades));
    OB_CHECK(!b.add_order(limit(4, ob::Side::BID, 103, 5), trades)); // full
    OB_CHECK(ok(b));

    // Freed capacity is reusable.
    b.cancel_order(2);
    OB_CHECK(b.add_order(limit(5, ob::Side::BID, 104, 5), trades));
    OB_CHECK(ok(b));
}

OB_TEST(book_invariants_survive_churn)
{
    ob::OrderBook b(9500, 10500, 4096);
    std::vector<ob::TradeReport> trades;

    for (int i = 1; i <= 2000; ++i) {
        const ob::Side s = (i % 2 == 0) ? ob::Side::BID : ob::Side::ASK;
        const int px = 10000 + (i % 7) - 3;
        b.add_order(limit(static_cast<uint64_t>(i), s, px, 1 + (i % 5)), trades);
        if (i % 37 == 0) b.cancel_order(static_cast<uint64_t>(i - 20));
        if (!ok(b)) {
            OB_FAIL(std::string("invariant broken: ") + invariant_err);
            return;
        }
    }
    OB_CHECK(b.best_bid() < b.best_ask());
}
