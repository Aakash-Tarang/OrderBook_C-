/*
 * benchmark.cpp – Single-thread micro-benchmark for the optimised engine.
 *
 * Methodology (corrected vs the phase-2/3/4 benchmarks – see report §7):
 *
 *   1. Per-op samples use ob::read_counter() (RDTSCP on x86-64, CNTVCT_EL0
 *      on ARM64/Apple Silicon) instead of two high_resolution_clock::now()
 *      calls.  The empty-bracket overhead is measured and printed, and is
 *      typically an order of magnitude below the vDSO clock it replaces.
 *
 *   2. NO outlier filtering.  The old harness deleted every sample outside
 *      Tukey fences (Q1-3·IQR .. Q3+3·IQR) before computing percentiles –
 *      which hid exactly the tail behaviour a matching engine is judged on.
 *      All percentiles below are computed on the raw sample set.
 *
 *   3. Block timing: after the per-op pass, the whole workload is re-run in
 *      blocks timed with steady_clock, giving throughput with near-zero
 *      measurement overhead per op.
 *
 *   4. `--with-reference` additionally runs the naive ob::ReferenceBook on
 *      the same workloads, so the optimised engine is compared against a
 *      faithful third-implementation stand-in *on the same machine* rather
 *      than against numbers copied from an older report.
 *
 * Workloads mirror archive/04_cplus_fourth/benchmark.cpp exactly (same
 * sizes, cancel ratio, price ranges, cluster structure, seeds) so results
 * remain comparable with the historical tables.
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <numeric>
#include <random>
#include <string>
#include <tuple>
#include <vector>

#include "ob/orderbook.h"
#include "ob/reference_book.h"
#include "ob/timing.h"

namespace {


// POSIX: create the parent directory of `path` if missing (single level).
// Both executables write into results/ by default; the dir may not exist on
// a fresh checkout.
static void ensure_parent_dir(const std::string& path)
{
    const std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0) return;
    std::string dir = path.substr(0, slash);
    ::mkdir(dir.c_str(), 0755);  // EEXIST is fine; errors surface at fopen
}

struct BenchOrder {
    ob::Order order;
    bool      is_cancel = false;
    uint64_t  cancel_id = 0;
};

// ── Workload generation (identical parameters to phase 4) ────────────────────

std::vector<BenchOrder> generate_dense_workload(
        int n_orders, double cancel_ratio, int price_min, int price_max, uint64_t seed)
{
    std::mt19937_64                    rng(seed);
    std::uniform_int_distribution<int> price_dist(price_min, price_max);
    std::uniform_int_distribution<int> qty_dist(1, 100);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_real_distribution<>   action_dist(0.0, 1.0);

    std::vector<BenchOrder> workload;
    workload.reserve(static_cast<size_t>(n_orders));

    std::vector<uint64_t> live_ids;
    uint64_t next_id = 1;

    for (int i = 0; i < n_orders; ++i) {
        BenchOrder bw;
        if (!live_ids.empty() && action_dist(rng) < cancel_ratio) {
            std::uniform_int_distribution<size_t> idx_dist(0, live_ids.size() - 1);
            const size_t idx = idx_dist(rng);
            bw.is_cancel = true;
            bw.cancel_id = live_ids[idx];
            live_ids.erase(live_ids.begin() + static_cast<std::ptrdiff_t>(idx));
        } else {
            bw.is_cancel   = false;
            bw.order.id    = next_id++;
            bw.order.side  = (side_dist(rng) == 0) ? ob::Side::BID : ob::Side::ASK;
            bw.order.price = price_dist(rng);
            bw.order.qty   = qty_dist(rng);
            bw.order.type  = ob::OrderType::LIMIT;
            live_ids.push_back(bw.order.id);
        }
        workload.push_back(bw);
    }
    return workload;
}

std::vector<BenchOrder> generate_sparse_workload(
        int n_orders, double cancel_ratio, int price_min, int price_max,
        int n_clusters, int cluster_jitter, uint64_t seed)
{
    std::mt19937_64                        rng(seed);
    std::uniform_int_distribution<int>     cluster_dist(0, n_clusters - 1);
    std::uniform_int_distribution<int>     jitter_dist(-cluster_jitter, cluster_jitter);
    std::uniform_int_distribution<int>     qty_dist(1, 100);
    std::uniform_int_distribution<int>     side_dist(0, 1);
    std::uniform_real_distribution<>       action_dist(0.0, 1.0);

    std::vector<int> cluster_centers(static_cast<size_t>(n_clusters));
    {
        const long long span = static_cast<long long>(price_max - price_min);
        for (int c = 0; c < n_clusters; ++c)
            cluster_centers[static_cast<size_t>(c)] =
                price_min + static_cast<int>(span * (c + 0.5) / n_clusters);
    }

    std::vector<BenchOrder> workload;
    workload.reserve(static_cast<size_t>(n_orders));

    std::vector<uint64_t> live_ids;
    uint64_t next_id = 1;

    for (int i = 0; i < n_orders; ++i) {
        BenchOrder bw;
        if (!live_ids.empty() && action_dist(rng) < cancel_ratio) {
            std::uniform_int_distribution<size_t> idx_dist(0, live_ids.size() - 1);
            const size_t idx = idx_dist(rng);
            bw.is_cancel = true;
            bw.cancel_id = live_ids[idx];
            live_ids.erase(live_ids.begin() + static_cast<std::ptrdiff_t>(idx));
        } else {
            const int center = cluster_centers[static_cast<size_t>(cluster_dist(rng))];
            const int price =
                std::clamp(center + jitter_dist(rng), price_min, price_max);

            bw.is_cancel   = false;
            bw.order.id    = next_id++;
            bw.order.side  = (side_dist(rng) == 0) ? ob::Side::BID : ob::Side::ASK;
            bw.order.price = price;
            bw.order.qty   = qty_dist(rng);
            bw.order.type  = ob::OrderType::LIMIT;
            live_ids.push_back(bw.order.id);
        }
        workload.push_back(bw);
    }
    return workload;
}

// ── Statistics (raw, unfiltered) ─────────────────────────────────────────────

struct Stats {
    double mean = 0, p50 = 0, p90 = 0, p99 = 0, p999 = 0, p9999 = 0;
    double min = 0, max = 0;
    size_t n = 0;
};

double percentile_sorted(const std::vector<double>& sorted, double p)
{
    if (sorted.empty()) return 0.0;
    const double idx = p / 100.0 * static_cast<double>(sorted.size() - 1);
    const auto   lo  = static_cast<size_t>(idx);
    const double frac = idx - static_cast<double>(lo);
    if (lo + 1 >= sorted.size()) return sorted.back();
    return sorted[lo] * (1.0 - frac) + sorted[lo + 1] * frac;
}

Stats make_stats(std::vector<double> samples)
{
    Stats s;
    s.n = samples.size();
    if (samples.empty()) return s;
    std::sort(samples.begin(), samples.end());
    const double sum = std::accumulate(samples.begin(), samples.end(), 0.0);
    s.mean  = sum / static_cast<double>(samples.size());
    s.min   = samples.front();
    s.max   = samples.back();
    s.p50   = percentile_sorted(samples, 50.0);
    s.p90   = percentile_sorted(samples, 90.0);
    s.p99   = percentile_sorted(samples, 99.0);
    s.p999  = percentile_sorted(samples, 99.9);
    s.p9999 = percentile_sorted(samples, 99.99);
    return s;
}

// ── Per-op timed run ─────────────────────────────────────────────────────────

struct PerOpResult {
    std::vector<double> all_ns;      // every op (add + cancel)
    std::vector<double> add_ns;
    std::vector<double> cancel_ns;
    uint64_t            trades = 0;
};

PerOpResult run_fast(const std::vector<BenchOrder>& wl, int warmup,
                     int price_min, int price_max)
{
    ob::OrderBook book(price_min, price_max, wl.size() + 1024);
    std::vector<ob::TradeReport> trades;   // reused → zero-alloc hot path
    trades.reserve(1024);

    PerOpResult r;
    r.all_ns.reserve(wl.size());
    r.add_ns.reserve(wl.size());
    r.cancel_ns.reserve(wl.size());

    for (size_t i = 0; i < wl.size(); ++i) {
        const BenchOrder& bw = wl[i];

        const uint64_t t0 = ob::read_counter();
        if (bw.is_cancel) {
            book.cancel_order(bw.cancel_id);
        } else {
            trades.clear();
            book.add_order(bw.order, trades);
            r.trades += trades.size();
        }
        const uint64_t t1 = ob::read_counter();

        if (static_cast<int>(i) >= warmup) {
            const double ns = ob::counter_to_ns(t1 - t0);
            r.all_ns.push_back(ns);
            (bw.is_cancel ? r.cancel_ns : r.add_ns).push_back(ns);
        }
    }
    return r;
}

PerOpResult run_reference(const std::vector<BenchOrder>& wl, int warmup)
{
    ob::ReferenceBook book;
    std::vector<ob::TradeReport> trades;
    trades.reserve(1024);

    PerOpResult r;
    r.all_ns.reserve(wl.size());
    r.add_ns.reserve(wl.size());
    r.cancel_ns.reserve(wl.size());

    for (size_t i = 0; i < wl.size(); ++i) {
        const BenchOrder& bw = wl[i];

        const uint64_t t0 = ob::read_counter();
        if (bw.is_cancel) {
            book.cancel_order(bw.cancel_id);
        } else {
            trades.clear();
            book.add_order(bw.order, trades);
            r.trades += trades.size();
        }
        const uint64_t t1 = ob::read_counter();

        if (static_cast<int>(i) >= warmup) {
            const double ns = ob::counter_to_ns(t1 - t0);
            r.all_ns.push_back(ns);
            (bw.is_cancel ? r.cancel_ns : r.add_ns).push_back(ns);
        }
    }
    return r;
}

// ── Block timing (throughput with ~zero per-op overhead) ─────────────────────

// Runs `ops_per_block` operations on a FRESH book per block (construction
// happens outside the timed window).  Returns the median of block means,
// which is robust against interference spikes.
double block_ns_per_op(const std::vector<BenchOrder>& wl, int blocks,
                       int ops_per_block, int price_min, int price_max)
{
    std::vector<double> block_means;
    block_means.reserve(static_cast<size_t>(blocks));
    const int n = std::min<int>(ops_per_block, static_cast<int>(wl.size()));

    for (int b = 0; b < blocks; ++b) {
        ob::OrderBook book(price_min, price_max,
                           static_cast<size_t>(n) + 1024);
        std::vector<ob::TradeReport> trades;

        const uint64_t t0 = ob::steady_now_ns();
        for (int i = 0; i < n; ++i) {
            const BenchOrder& bw = wl[static_cast<size_t>(i)];
            if (bw.is_cancel) book.cancel_order(bw.cancel_id);
            else {
                trades.clear();
                book.add_order(bw.order, trades);
            }
        }
        const uint64_t t1 = ob::steady_now_ns();

        block_means.push_back(static_cast<double>(t1 - t0) /
                              static_cast<double>(n));
    }
    std::sort(block_means.begin(), block_means.end());
    return block_means[static_cast<size_t>(blocks) / 2];
}

// ── Printing ─────────────────────────────────────────────────────────────────

void print_row(const char* engine, const char* workload, const char* op,
               const Stats& s)
{
    std::printf("  %-9s %-7s %-7s n=%-7zu mean=%9.1f p50=%8.1f p90=%8.1f "
                "p99=%8.1f p999=%9.1f max=%10.1f\n",
                engine, workload, op, s.n, s.mean, s.p50, s.p90, s.p99,
                s.p999, s.max);
}

void write_summary_csv(const std::string& path,
                       const std::vector<std::tuple<
                           std::string, std::string, std::string, Stats>>& rows)
{
    ensure_parent_dir(path);
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return;
    std::fprintf(f, "engine,workload,op,n,mean_ns,p50_ns,p90_ns,p99_ns,"
                    "p999_ns,p9999_ns,min_ns,max_ns\n");
    for (const auto& [engine, workload, op, s] : rows) {
        std::fprintf(f, "%s,%s,%s,%zu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
                     engine.c_str(), workload.c_str(), op.c_str(), s.n,
                     s.mean, s.p50, s.p90, s.p99, s.p999, s.p9999, s.min, s.max);
    }
    std::fclose(f);
}

void write_raw_csv(const std::string& path,
                   const std::vector<std::tuple<std::string, std::string,
                                                std::vector<double>, char>>& series)
{
    ensure_parent_dir(path);
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return;
    std::fprintf(f, "engine,workload,op_index,op_type,latency_ns\n");
    for (const auto& [engine, workload, samples, type] : series) {
        for (size_t i = 0; i < samples.size(); ++i)
            std::fprintf(f, "%s,%s,%zu,%c,%.3f\n", engine.c_str(),
                         workload.c_str(), i, type, samples[i]);
    }
    std::fclose(f);
}

} // namespace

int main(int argc, char** argv)
{
    bool with_reference = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--with-reference") == 0) with_reference = true;

    // ── Workload parameters (identical to archive/04_cplus_fourth) ──────────
    constexpr int    DENSE_N         = 300'000;
    constexpr int    DENSE_WARMUP    = 5'000;
    constexpr double CANCEL_RATIO    = 0.40;
    constexpr int    DENSE_MIN       = 9'900;
    constexpr int    DENSE_MAX       = 10'100;

    constexpr int    SPARSE_N        = 150'000;
    constexpr int    SPARSE_WARMUP   = 2'000;
    constexpr int    SPARSE_MIN      = 0;
    constexpr int    SPARSE_MAX      = 200'000;
    constexpr int    SPARSE_CLUSTERS = 20;
    constexpr int    SPARSE_JITTER   = 3;

    std::printf("=== OrderBook micro-benchmark ===\n");
    std::printf("counter: %s | ns/tick: %.4f | bracket overhead: %.1f ns\n",
                ob::has_hw_counter() ? "hardware" : "steady_clock fallback",
                ob::counter_resolution_ns(),
                ob::counter_bracket_overhead_ns());
#ifdef NDEBUG
    std::printf("build: Release (asserts off)\n");
#else
    std::printf("build: Debug (asserts ON – do not quote these numbers)\n");
#endif

    std::printf("generating workloads...\n");
    auto dense  = generate_dense_workload(DENSE_N, CANCEL_RATIO,
                                          DENSE_MIN, DENSE_MAX, 42);
    auto sparse = generate_sparse_workload(SPARSE_N, CANCEL_RATIO,
                                           SPARSE_MIN, SPARSE_MAX,
                                           SPARSE_CLUSTERS, SPARSE_JITTER, 43);

    struct Named {
        const char*                     name;
        const std::vector<BenchOrder>*  wl;
        int                             warmup;
        int                             price_min;
        int                             price_max;
    };
    const Named runs[] = {
        {"dense",  &dense,  DENSE_WARMUP,  DENSE_MIN,  DENSE_MAX},
        {"sparse", &sparse, SPARSE_WARMUP, SPARSE_MIN, SPARSE_MAX},
    };

    std::vector<std::tuple<std::string, std::string, std::string, Stats>> summary;
    std::vector<std::tuple<std::string, std::string, std::vector<double>, char>> raw;

    for (const Named& run : runs) {
        std::printf("\n-- %s workload (%zu ops) --\n",
                    run.name, run.wl->size());

        auto fast = run_fast(*run.wl, run.warmup, run.price_min, run.price_max);
        print_row("fast", run.name, "all",    make_stats(fast.all_ns));
        print_row("fast", run.name, "add",    make_stats(fast.add_ns));
        print_row("fast", run.name, "cancel", make_stats(fast.cancel_ns));
        std::printf("  fast trades executed: %llu\n",
                    static_cast<unsigned long long>(fast.trades));

        summary.emplace_back("fast", run.name, "all",    make_stats(fast.all_ns));
        summary.emplace_back("fast", run.name, "add",    make_stats(fast.add_ns));
        summary.emplace_back("fast", run.name, "cancel", make_stats(fast.cancel_ns));
        raw.emplace_back("fast", run.name, fast.all_ns, 'M');

        if (with_reference) {
            auto ref = run_reference(*run.wl, run.warmup);
            print_row("ref", run.name, "all",    make_stats(ref.all_ns));
            print_row("ref", run.name, "add",    make_stats(ref.add_ns));
            print_row("ref", run.name, "cancel", make_stats(ref.cancel_ns));
            std::printf("  ref  trades executed: %llu (must equal fast)\n",
                        static_cast<unsigned long long>(ref.trades));
            if (ref.trades != fast.trades)
                std::printf("  !! TRADE COUNT MISMATCH – correctness bug\n");

            summary.emplace_back("ref", run.name, "all",    make_stats(ref.all_ns));
            summary.emplace_back("ref", run.name, "add",    make_stats(ref.add_ns));
            summary.emplace_back("ref", run.name, "cancel", make_stats(ref.cancel_ns));
            raw.emplace_back("ref", run.name, ref.all_ns, 'M');
        }
    }

    // ── Block throughput (fresh book per block, near-zero overhead) ─────────
    std::printf("\n-- block throughput (median ns/op over %d blocks x %d ops) --\n",
                50, 20'000);
    {
        const double dense_block =
            block_ns_per_op(dense, 50, 20'000, DENSE_MIN, DENSE_MAX);
        std::printf("  dense : %8.1f ns/op\n", dense_block);

        const double sparse_block =
            block_ns_per_op(sparse, 50, 20'000, SPARSE_MIN, SPARSE_MAX);
        std::printf("  sparse: %8.1f ns/op\n", sparse_block);
    }

    write_summary_csv("results/bench_summary.csv", summary);
    write_raw_csv("results/bench_raw.csv", raw);
    std::printf("\nwrote results/bench_summary.csv and results/bench_raw.csv\n");
    return 0;
}
