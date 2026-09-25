// Differential fuzz test – optimised OrderBook vs the naive ReferenceBook.
//
// Both books receive byte-identical random operation streams.  After every
// operation we compare:
//   1. the full trade sequence produced (ids, prices, quantities, order)
//   2. the complete book state (every level, total quantity, FIFO contents)
//   3. best bid / best ask
//   4. live order count
// Any divergence fails the test with the offending seed printed, so the
// failure is reproducible.
#include "test_framework.h"

#include <random>
#include <string>
#include <vector>

#include "ob/orderbook.h"
#include "ob/reference_book.h"

namespace {

struct FuzzConfig {
    uint64_t seed;
    int      n_steps;
    int      price_min;
    int      price_max;     // inclusive, prices are uniform in [min, max]
    double   cancel_ratio;
    double   market_ratio;
    int      max_qty;
};

bool compare_state(const ob::OrderBook& fast, const ob::ReferenceBook& ref,
                   std::string& err)
{
    if (fast.best_bid() != ref.best_bid()) {
        err = "best_bid diverged: fast=" + std::to_string(fast.best_bid()) +
              " ref=" + std::to_string(ref.best_bid());
        return false;
    }
    if (fast.best_ask() != ref.best_ask()) {
        err = "best_ask diverged: fast=" + std::to_string(fast.best_ask()) +
              " ref=" + std::to_string(ref.best_ask());
        return false;
    }
    if (fast.live_orders() != ref.live_orders()) {
        err = "live_orders diverged: fast=" +
              std::to_string(fast.live_orders()) + " ref=" +
              std::to_string(ref.live_orders());
        return false;
    }

    const ob::Side sides[2] = {ob::Side::BID, ob::Side::ASK};
    for (ob::Side side : sides) {
        if (fast.dump_levels(side) != ref.dump_levels(side)) {
            auto f = fast.dump_levels(side);
            auto r = ref.dump_levels(side);
            err = (side == ob::Side::BID ? std::string("bid") : "ask") +
                  std::string(" levels diverged: fast has ") +
                  std::to_string(f.size()) + " levels, ref has " +
                  std::to_string(r.size());
            return false;
        }
        for (const auto& [price, total] : ref.dump_levels(side)) {
            if (fast.dump_level_orders(side, price) !=
                ref.dump_level_orders(side, price)) {
                err = (side == ob::Side::BID ? std::string("bid") : "ask") +
                      std::string(" FIFO diverged at price ") +
                      std::to_string(price);
                return false;
            }
        }
    }

    std::string inv_err;
    if (!fast.verify_invariants(inv_err)) {
        err = "invariant violation: " + inv_err;
        return false;
    }
    return true;
}

bool run_fuzz(const FuzzConfig& cfg, std::string& err)
{
    ob::OrderBook     fast(cfg.price_min, cfg.price_max,
                           static_cast<size_t>(cfg.n_steps) + 16);
    ob::ReferenceBook ref;

    std::mt19937_64 rng(cfg.seed);
    std::uniform_int_distribution<int> price_dist(cfg.price_min, cfg.price_max);
    std::uniform_int_distribution<int> qty_dist(1, cfg.max_qty);
    std::uniform_real_distribution<double> action_dist(0.0, 1.0);
    std::uniform_int_distribution<int> side_dist(0, 1);

    std::vector<uint64_t> live_ids;
    uint64_t next_id = 1;
    std::vector<ob::TradeReport> trades_fast, trades_ref;

    for (int step = 0; step < cfg.n_steps; ++step) {
        trades_fast.clear();
        trades_ref.clear();

        const double roll = action_dist(rng);
        const bool want_cancel = (!live_ids.empty() && roll < cfg.cancel_ratio);
        const bool want_market = (!want_cancel && roll < cfg.cancel_ratio +
                                  cfg.market_ratio);

        if (want_cancel) {
            const size_t idx = static_cast<size_t>(
                rng() % static_cast<uint64_t>(live_ids.size()));
            const uint64_t id = live_ids[idx];
            live_ids[idx] = live_ids.back();
            live_ids.pop_back();

            const bool a = fast.cancel_order(id);
            const bool b = ref.cancel_order(id);
            if (a != b) {
                err = "cancel return diverged at step " +
                      std::to_string(step) + " id " + std::to_string(id);
                return false;
            }
        } else {
            ob::Order o;
            o.id    = next_id++;
            o.side  = (side_dist(rng) == 0) ? ob::Side::BID : ob::Side::ASK;
            o.qty   = qty_dist(rng);
            o.type  = want_market ? ob::OrderType::MARKET : ob::OrderType::LIMIT;
            o.price = want_market ? 0 : price_dist(rng);

            // Trades are compared only when both books accepted; acceptance
            // must agree first.
            const bool a = fast.add_order(o, trades_fast);
            const bool b = ref.add_order(o, trades_ref);
            if (a != b) {
                err = "add accept diverged at step " + std::to_string(step);
                return false;
            }
            if (a) {
                if (trades_fast.size() != trades_ref.size()) {
                    err = "trade count diverged at step " +
                          std::to_string(step) + ": fast=" +
                          std::to_string(trades_fast.size()) + " ref=" +
                          std::to_string(trades_ref.size());
                    return false;
                }
                for (size_t i = 0; i < trades_fast.size(); ++i) {
                    const auto& f = trades_fast[i];
                    const auto& r = trades_ref[i];
                    if (f.bid_id != r.bid_id || f.ask_id != r.ask_id ||
                        f.price != r.price || f.qty != r.qty) {
                        err = "trade " + std::to_string(i) +
                              " diverged at step " + std::to_string(step) +
                              ": fast=(" + std::to_string(f.bid_id) + "," +
                              std::to_string(f.ask_id) + "," +
                              std::to_string(f.price) + "," +
                              std::to_string(f.qty) + ") ref=(" +
                              std::to_string(r.bid_id) + "," +
                              std::to_string(r.ask_id) + "," +
                              std::to_string(r.price) + "," +
                              std::to_string(r.qty) + ")";
                        return false;
                    }
                }
                if (o.type == ob::OrderType::LIMIT)
                    live_ids.push_back(o.id);
            }
        }

        if (!compare_state(fast, ref, err)) {
            err += " (seed=" + std::to_string(cfg.seed) + " step=" +
                   std::to_string(step) + ")";
            return false;
        }
    }
    return true;
}

} // namespace

OB_TEST(differential_fuzz_dense_book)
{
    // Tight price range → the book is constantly crossed, heavy matching.
    for (uint64_t seed : {1ULL, 2ULL, 3ULL}) {
        std::string err;
        const bool okf = run_fuzz(
            {seed, 3000, /*price_min=*/9900, /*price_max=*/10100,
             /*cancel_ratio=*/0.35, /*market_ratio=*/0.10, /*max_qty=*/50},
            err);
        if (!okf) { OB_FAIL(err); return; }
    }
}

OB_TEST(differential_fuzz_sparse_book)
{
    // Wide price range → huge liquidity voids; exercises the hierarchical
    // bitset far-walk against the tree-based reference.
    for (uint64_t seed : {11ULL, 12ULL}) {
        std::string err;
        const bool okf = run_fuzz(
            {seed, 2000, /*price_min=*/0, /*price_max=*/200000,
             /*cancel_ratio=*/0.45, /*market_ratio=*/0.05, /*max_qty=*/100},
            err);
        if (!okf) { OB_FAIL(err); return; }
    }
}

OB_TEST(differential_fuzz_two_tick_book)
{
    // Degenerate: every order sits on the same two prices.
    std::string err;
    const bool okf = run_fuzz(
        {21ULL, 2000, /*price_min=*/100, /*price_max=*/101,
         /*cancel_ratio=*/0.5, /*market_ratio=*/0.15, /*max_qty=*/5},
        err);
    if (!okf) { OB_FAIL(err); return; }
}

OB_TEST(differential_fuzz_market_order_storm)
{
    // Mostly market orders: the sweep paths get hammered.
    for (uint64_t seed : {31ULL, 32ULL, 33ULL, 34ULL}) {
        std::string err;
        const bool okf = run_fuzz(
            {seed, 1500, /*price_min=*/500, /*price_max=*/1500,
             /*cancel_ratio=*/0.10, /*market_ratio=*/0.60, /*max_qty=*/200},
            err);
        if (!okf) { OB_FAIL(err); return; }
    }
}

OB_TEST(differential_fuzz_long_run)
{
    std::string err;
    const bool okf = run_fuzz(
        {77ULL, 20000, /*price_min=*/9000, /*price_max=*/11000,
         /*cancel_ratio=*/0.55, /*market_ratio=*/0.05, /*max_qty=*/60},
        err);
    if (!okf) { OB_FAIL(err); return; }
}
