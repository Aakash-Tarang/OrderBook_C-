/*
 * pipeline_main.cpp – Multithreaded HFT matching-engine pipeline (target:
 * `matching_engine`).
 *
 * Architecture (unchanged shape from the fifth phase, hardened everywhere):
 *
 *   ┌──────────────────────┐       SPSC ring buffer        ┌─────────────────────────┐
 *   │  Thread 1 (Producer) │  ─────────────────────────►  │  Thread 2 (Consumer)    │
 *   │  Feed handler        │   BenchmarkOrder (+ingest)   │  Matching engine        │
 *   │  file → events       │                              │  pop → apply → measure  │
 *   └──────────────────────┘                              └─────────────────────────┘
 *
 * Improvements over the fifth phase:
 *
 *   1. Binary replay format (ob::replay) – the producer no longer parses
 *      text on the fly unless given a .csv.  CSV parsing itself was
 *      rewritten: whole-file read, in-place pointer parsing, manual integer
 *      conversion – no getline/substr/stoull/exceptions/allocations.
 *
 *   2. Portable timing (ob::timing.h): RDTSCP on x86-64, CNTVCT_EL0 on
 *      Apple Silicon – replaces high_resolution_clock, whose ~20–30 ns
 *      overhead per now() call was comparable to the op being measured.
 *
 *   3. Tick-to-trade latency: the producer stamps each event at enqueue
 *      time; the consumer reports both the engine-op latency (pop→done)
 *      and the full tick-to-trade (enqueue→done).
 *
 *   4. Honest statistics: raw percentiles up to P99.99, NO Tukey outlier
 *      filtering, measurement overhead printed alongside.
 *
 *   5. Trade reports land in one reused vector – zero allocations in the
 *      steady-state consumer loop.
 *
 * Latency definitions
 * ───────────────────
 *   op latency     : consumer-side bracket around the OrderBook call only
 *   tick-to-trade  : producer enqueue stamp → consumer completion
 *   (queueing delay = tick-to-trade − op latency)
 */

#include "ob/orderbook.h"
#include "ob/replay_format.h"
#include "ob/ring_buffer.h"
#include "ob/timing.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <string>
#include <thread>
#include <vector>

#ifdef __linux__
#  include <pthread.h>
#  include <sched.h>
#endif


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

// ── Ring payload ─────────────────────────────────────────────────────────────

struct BenchmarkOrder {
    ob::Order order     = {};   // ADD operations
    uint64_t  cancel_id = 0;    // CANCEL operations
    uint64_t  ingest_ts = 0;    // producer counter stamp (tick domain)
    bool      is_cancel = false;
};
static_assert(sizeof(BenchmarkOrder) <= 64,
              "ring payload should stay within one cache line");

static constexpr std::size_t RING_CAPACITY = 65'536;   // power of two
static std::atomic<bool> producer_done{false};

// ── Core pinning (Linux; a documented no-op elsewhere, incl. macOS) ─────────

static void pin_thread_to_core(int core_id)
{
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0)
        std::fprintf(stderr, "[warn] could not pin to core %d\n", core_id);
#else
    (void)core_id;
    // macOS has no first-class thread affinity API; thread policies are set
    // implicitly by the QoS scheduler.  Numbers remain comparable run-to-run
    // on the same machine but cross-core placement is not controllable.
#endif
}

// ── Input loading ────────────────────────────────────────────────────────────

// Whole-file read – one allocation, no getline scanning.
static bool read_whole_file(const std::string& path, std::vector<char>& out)
{
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size < 0) { std::fclose(f); return false; }
    out.resize(static_cast<size_t>(size));
    const size_t got = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    return got == out.size();
}

// Manual integer parse – no exceptions, no locale, no allocation.
static const char* parse_u64(const char* p, uint64_t& out)
{
    uint64_t v = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + static_cast<uint64_t>(*p - '0'); ++p; }
    out = v;
    return p;
}

static const char* parse_i32(const char* p, int32_t& out)
{
    int32_t v = 0;
    bool neg = false;
    if (*p == '-') { neg = true; ++p; }
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); ++p; }
    out = neg ? -v : v;
    return p;
}

static bool parse_csv_line(const char* p, const char* end, BenchmarkOrder& o)
{
    // Format: action,order_id,side,price,qty  with \n or \r\n terminators.
    if (p >= end) return false;
    const char action = *p++;
    if (action != 'A' && action != 'C') return false;   // header / blank
    if (p >= end || *p != ',') return false;
    ++p;

    uint64_t id = 0;
    p = parse_u64(p, id);
    if (p >= end || *p != ',') return false;
    ++p;

    // side: "BID" or "ASK"
    uint8_t side;
    if (end - p >= 3 && p[0] == 'B' && p[1] == 'I' && p[2] == 'D') { side = 0; p += 3; }
    else if (end - p >= 3 && p[0] == 'A' && p[1] == 'S' && p[2] == 'K') { side = 1; p += 3; }
    else return false;
    if (p >= end || *p != ',') return false;
    ++p;

    int32_t price = 0, qty = 0;
    p = parse_i32(p, price);
    if (p >= end || *p != ',') return false;
    ++p;
    p = parse_i32(p, qty);

    // Tolerate \r before the line end; trailing junk rejects the line.
    while (p < end && (*p == '\r')) ++p;
    if (p != end) return false;

    if (action == 'C') {
        o.is_cancel = true;
        o.cancel_id = id;
    } else {
        o.is_cancel = false;
        o.order.id    = id;
        o.order.price = price;
        o.order.qty   = qty;
        o.order.side  = (side == 0) ? ob::Side::BID : ob::Side::ASK;
        o.order.type  = ob::OrderType::LIMIT;
        o.cancel_id   = 0;
    }
    return true;
}

// Returns false on I/O failure (message printed).
static bool load_events(const std::string& path, std::vector<BenchmarkOrder>& out)
{
    if (path.size() > 4 && path.compare(path.size() - 4, 4, ".bin") == 0) {
        ob::replay::Reader reader(path);
        if (!reader.ok()) {
            std::fprintf(stderr, "cannot open replay file '%s'\n", path.c_str());
            return false;
        }
        ob::replay::Event ev;
        out.reserve(static_cast<size_t>(reader.remaining()));
        while (reader.next(ev)) {
            BenchmarkOrder o;
            o.ingest_ts = 0;
            if (ev.action == ob::replay::kCancel) {
                o.is_cancel = true;
                o.cancel_id = ev.order_id;
            } else {
                o.is_cancel       = false;
                o.order.id        = ev.order_id;
                o.order.price     = ev.price;
                o.order.qty       = ev.qty;
                o.order.side      = (ev.side == ob::replay::kBid) ? ob::Side::BID
                                                                  : ob::Side::ASK;
                o.order.type      = ob::OrderType::LIMIT;
            }
            out.push_back(o);
        }
        if (reader.fail()) {
            std::fprintf(stderr, "replay file '%s' is truncated/corrupt\n",
                         path.c_str());
            return false;
        }
        return true;
    }

    // CSV path – parsed once, up front, with the fast parser.
    std::vector<char> buf;
    if (!read_whole_file(path, buf)) {
        std::fprintf(stderr, "cannot open csv file '%s'\n", path.c_str());
        return false;
    }

    const char* p   = buf.data();
    const char* end = buf.data() + buf.size();
    out.reserve(buf.size() / 40 + 16);

    while (p < end) {
        const char* nl = static_cast<const char*>(
            memchr(p, '\n', static_cast<size_t>(end - p)));
        const char* line_end = nl ? nl : end;
        BenchmarkOrder o;
        if (parse_csv_line(p, line_end, o)) out.push_back(o);
        p = nl ? nl + 1 : end;
    }
    return true;
}

// ── Producer ─────────────────────────────────────────────────────────────────

static void producer_thread(ob::SPSCRingBuffer<BenchmarkOrder, RING_CAPACITY>& ring,
                            const std::vector<BenchmarkOrder>& events)
{
    pin_thread_to_core(0);

    for (const BenchmarkOrder& ev : events) {
        BenchmarkOrder item = ev;
        item.ingest_ts = ob::read_counter();   // tick-to-trade starts here
        while (!ring.push(item)) ob::spin_pause();
    }
    producer_done.store(true, std::memory_order_release);
}

// ── Statistics ───────────────────────────────────────────────────────────────

static double percentile_sorted(std::vector<double> s, double p)
{
    if (s.empty()) return 0.0;
    std::sort(s.begin(), s.end());
    const double idx = p / 100.0 * static_cast<double>(s.size() - 1);
    const auto   lo  = static_cast<size_t>(idx);
    const double frac = idx - static_cast<double>(lo);
    if (lo + 1 >= s.size()) return s.back();
    return s[lo] * (1.0 - frac) + s[lo + 1] * frac;
}

struct Summary {
    std::vector<double> samples;
    const char*         label;
};

static void print_and_write(const std::vector<Summary>& summaries,
                            const std::string& summary_path)
{
    ensure_parent_dir(summary_path);
    std::FILE* f = std::fopen(summary_path.c_str(), "w");
    if (f) std::fprintf(f, "metric,n,mean_ns,p50_ns,p90_ns,p99_ns,p999_ns,"
                           "p9999_ns,min_ns,max_ns\n");

    for (const Summary& s : summaries) {
        if (s.samples.empty()) continue;
        std::vector<double> sorted(s.samples);
        std::sort(sorted.begin(), sorted.end());
        double sum = 0.0;
        for (double v : sorted) sum += v;
        const double mean = sum / static_cast<double>(sorted.size());
        const double p50  = percentile_sorted(sorted, 50.0);
        const double p90  = percentile_sorted(sorted, 90.0);
        const double p99  = percentile_sorted(sorted, 99.0);
        const double p999 = percentile_sorted(sorted, 99.9);
        const double p9999= percentile_sorted(sorted, 99.99);
        const double mn   = sorted.front();
        const double mx   = sorted.back();

        std::printf("  %-16s n=%-8zu mean=%9.1f p50=%8.1f p90=%8.1f p99=%8.1f "
                    "p999=%9.1f p9999=%9.1f min=%8.1f max=%10.1f ns\n",
                    s.label, sorted.size(), mean, p50, p90, p99, p999, p9999, mn, mx);
        if (f)
            std::fprintf(f, "%s,%zu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
                         s.label, sorted.size(), mean, p50, p90, p99, p999,
                         p9999, mn, mx);
    }
    if (f) std::fclose(f);
}

// ── Consumer ─────────────────────────────────────────────────────────────────

static void consumer_thread(ob::SPSCRingBuffer<BenchmarkOrder, RING_CAPACITY>& ring,
                            int price_min, int price_max, std::size_t pool_cap,
                            int warmup, const std::string& out_prefix)
{
    pin_thread_to_core(1);

    ob::OrderBook book(price_min, price_max, pool_cap);
    std::vector<ob::TradeReport> trades;
    trades.reserve(1024);

    std::vector<double> op_ns, ttt_ns;
    op_ns.reserve(600'000);
    ttt_ns.reserve(600'000);

    uint64_t trades_executed = 0;
    int      ops_seen        = 0;

    BenchmarkOrder item;
    auto apply_one = [&](BenchmarkOrder& it) {
        const uint64_t t0 = ob::read_counter();
        if (it.is_cancel) {
            book.cancel_order(it.cancel_id);
        } else {
            trades.clear();
            book.add_order(it.order, trades);
            trades_executed += trades.size();
        }
        const uint64_t t1 = ob::read_counter();

        if (++ops_seen > warmup) {
            op_ns.push_back(ob::counter_to_ns(t1 - t0));
            // Tick-to-trade: producer stamp → completion.  Guard against a
            // torn/stale stamp (never expected in-process) with a max().
            const double t = ob::counter_to_ns(t1 - it.ingest_ts);
            ttt_ns.push_back(t > 0.0 ? t : 0.0);
        }
    };

    bool done = false;
    while (!done) {
        if (!ring.pop(item)) {
            if (producer_done.load(std::memory_order_acquire)) {
                while (ring.pop(item)) apply_one(item);  // drain
                done = true;
            } else {
                ob::spin_pause();
            }
            continue;
        }
        apply_one(item);
    }

    std::printf("[consumer] ops=%d (recorded=%zu) trades=%llu\n",
                ops_seen, op_ns.size(), static_cast<unsigned long long>(trades_executed));
    if (!book.empty_bids()) std::printf("[consumer] final best bid: %d\n", book.best_bid());
    if (!book.empty_asks()) std::printf("[consumer] final best ask: %d\n", book.best_ask());

    // Raw per-op CSV (tick-to-trade column included).
    {
        ensure_parent_dir(out_prefix + "_raw.csv");
        std::FILE* f = std::fopen((out_prefix + "_raw.csv").c_str(), "w");
        if (f) {
            std::fprintf(f, "op_index,op_latency_ns,tick_to_trade_ns\n");
            for (size_t i = 0; i < op_ns.size(); ++i)
                std::fprintf(f, "%zu,%.3f,%.3f\n", i, op_ns[i], ttt_ns[i]);
            std::fclose(f);
        }
    }

    std::vector<Summary> summaries = {
        {std::move(op_ns),  "op_latency"},
        {std::move(ttt_ns), "tick_to_trade"},
    };
    print_and_write(summaries, out_prefix + "_summary.csv");
}

// ── main ─────────────────────────────────────────────────────────────────────

int main(int argc, char** argv)
{
    std::string input  = "data/market_data_sample.csv";
    std::string prefix = "results/pipeline";
    int         warmup = 5'000;
    int         price_min = 1;
    int         price_max = 11'000;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : ""; };
        if      (a == "--input"  || a == "-i") input  = next();
        else if (a == "--prefix" || a == "-o") prefix = next();
        else if (a == "--warmup")              warmup = std::atoi(next());
        else if (a == "--price-min")           price_min = std::atoi(next());
        else if (a == "--price-max")           price_max = std::atoi(next());
        else {
            std::fprintf(stderr,
                "usage: %s [--input file.csv|.bin] [--prefix out] "
                "[--warmup n] [--price-min n] [--price-max n]\n", argv[0]);
            return 1;
        }
    }

    std::printf("=== HFT matching-engine pipeline ===\n");
    std::printf("input        : %s\n", input.c_str());
    std::printf("ring         : %zu slots\n", RING_CAPACITY);
    std::printf("counter      : %s | ns/tick %.4f | bracket %.1f ns\n",
                ob::has_hw_counter() ? "hardware" : "steady_clock fallback",
                ob::counter_resolution_ns(), ob::counter_bracket_overhead_ns());
#ifdef NDEBUG
    std::printf("build        : Release\n");
#else
    std::printf("build        : Debug (do not quote these numbers)\n");
#endif

    std::vector<BenchmarkOrder> events;
    if (!load_events(input, events)) return 1;
    std::printf("loaded       : %zu events\n", events.size());

    ob::SPSCRingBuffer<BenchmarkOrder, RING_CAPACITY> ring;

    const std::size_t pool_cap = 600'000; // ≥ max simultaneous live orders
    std::thread consumer([&] {
        consumer_thread(ring, price_min, price_max, pool_cap, warmup, prefix);
    });
    std::thread producer([&] {
        producer_thread(ring, events);
    });

    producer.join();
    consumer.join();

    std::printf("done. wrote %s_summary.csv and %s_raw.csv\n",
                prefix.c_str(), prefix.c_str());
    return 0;
}
