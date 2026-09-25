/*
 * main.cpp – Multithreaded HFT matching-engine pipeline.
 *
 * Architecture
 * ────────────
 *
 *   ┌──────────────────────┐       SPSC ring buffer        ┌─────────────────────────┐
 *   │  Thread 1 (Producer) │  ─────────────────────────►  │  Thread 2 (Consumer)    │
 *   │  "Network / Ingest"  │        BenchmarkOrder         │  "Matching Engine"      │
 *   │                      │                               │                         │
 *   │  Read CSV line       │                               │  pop() from ring        │
 *   │  Parse → struct      │                               │  t0 = now()             │
 *   │  push() to ring      │                               │  add_order / cancel     │
 *   └──────────────────────┘                               │  t1 = now()             │
 *                                                          │  record (t1 - t0) ns    │
 *                                                          └─────────────────────────┘
 *
 * Latency definition
 * ──────────────────
 * We measure the OrderBook operation latency from the moment pop() succeeds
 * (i.e., the data is in our hands) to the moment the operation returns.
 * This isolates the matching-engine cost from queue-wait time.
 *
 * Key design decisions
 * ────────────────────
 * 1. The ring buffer capacity is a power of two (65536) so index wrap
 *    uses a bitmask instead of modulo.
 *
 * 2. The producer signals "done" via an atomic done_ flag rather than a
 *    sentinel value in the ring, keeping the ring buffer generic.
 *
 * 3. Both threads are pinned to separate physical cores (if available)
 *    via POSIX pthread_setaffinity to minimise OS scheduling jitter.
 *    If affinity setting fails we print a warning and continue.
 *
 * 4. The consumer pre-reserves the latency vector to avoid heap
 *    reallocation during the hot path.
 *
 * 5. A warm-up period (WARMUP_OPS) lets branch predictors and caches
 *    reach steady state before we start recording latencies.
 */

#include "orderbook.h"  // p4::OrderBook, p4::Order, p4::Side, p4::OrderType
#include "ring_buffer.h"       // SPSCRingBuffer<T, Capacity>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// POSIX thread affinity – available on Linux.  Guard for portability.
#ifdef __linux__
#  include <pthread.h>
#  include <sched.h>
#endif

// ── Wrapper struct passed through the ring buffer ─────────────────────────────
//
// BenchmarkOrder carries either:
//   is_cancel == false  →  a new limit order in `order`
//   is_cancel == true   →  a cancellation; only `cancel_id` is used
//
// Keeping the struct small (≤ 64 bytes) means it fits in one cache line,
// so the producer's store and the consumer's load touch a single line.
struct BenchmarkOrder {
    p4::Order order     = {};   // populated for Add operations
    uint64_t  cancel_id = 0;   // populated for Cancel operations
    bool      is_cancel = false;
};
static_assert(sizeof(BenchmarkOrder) <= 64,
              "BenchmarkOrder exceeds one cache line – consider packing.");

// ── Ring buffer configuration ─────────────────────────────────────────────────
// 65536 slots × 48 bytes ≈ 3 MB – fits comfortably in L3.
// Power-of-two mandatory for SPSCRingBuffer's bitmask wrap.
static constexpr std::size_t RING_CAPACITY = 65'536;

using Ring = SPSCRingBuffer<BenchmarkOrder, RING_CAPACITY>;

// ── Pipeline parameters ───────────────────────────────────────────────────────
static constexpr int    PRICE_MIN      = 1;
static constexpr int    PRICE_MAX      = 11'000; // covers mid ± drift from generator
static constexpr std::size_t POOL_CAP  = 600'000; // max simultaneous live orders
static constexpr int    WARMUP_OPS     = 5'000;  // ops discarded before recording

// ── Shared termination flag ───────────────────────────────────────────────────
// The producer sets this after enqueuing its last item; the consumer spins
// on both the ring and this flag to know when to exit cleanly.
static std::atomic<bool> producer_done{false};

// ─────────────────────────────────────────────────────────────────────────────
// Pin the calling thread to a specific CPU core.
// On non-Linux platforms this is a no-op (with a warning).
// ─────────────────────────────────────────────────────────────────────────────
static void pin_thread_to_core(int core_id) {
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
        std::cerr << "[warn] Could not pin thread to core " << core_id
                  << " (errno=" << rc << "). Continuing without affinity.\n";
    }
#else
    (void)core_id;
    std::cerr << "[warn] Thread pinning not supported on this platform.\n";
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// percentile(sorted_vector, p)
// Linear interpolation, same method as the original benchmark.
// ─────────────────────────────────────────────────────────────────────────────
static double percentile(const std::vector<long long>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    double idx  = p / 100.0 * (static_cast<double>(sorted.size()) - 1.0);
    auto   lo   = static_cast<std::size_t>(idx);
    double frac = idx - static_cast<double>(lo);
    if (lo + 1 >= sorted.size()) return static_cast<double>(sorted.back());
    return static_cast<double>(sorted[lo]) * (1.0 - frac) +
           static_cast<double>(sorted[lo + 1]) * frac;
}

// ─────────────────────────────────────────────────────────────────────────────
// Tukey-fence outlier filter (same as original benchmark, fence=3.0).
// ─────────────────────────────────────────────────────────────────────────────
static std::vector<long long> filter_outliers(std::vector<long long> v,
                                               double fence = 3.0) {
    if (v.empty()) return v;
    std::sort(v.begin(), v.end());
    double q1  = percentile(v, 25.0);
    double q3  = percentile(v, 75.0);
    double iqr = q3 - q1;
    long long lo = static_cast<long long>(q1 - fence * iqr);
    long long hi = static_cast<long long>(q3 + fence * iqr);
    std::vector<long long> out;
    out.reserve(v.size());
    for (auto x : v)
        if (x >= lo && x <= hi) out.push_back(x);
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// parse_csv_line
//
// Parses one CSV row into a BenchmarkOrder.
// Expected format (no spaces around commas):
//   action,order_id,side,price,qty
//   A,1,BID,9998,50
//   C,3,ASK,10002,20    ← side/price/qty present but only cancel_id used
//
// Returns false if the line is malformed (e.g. header row).
// ─────────────────────────────────────────────────────────────────────────────
static bool parse_csv_line(const std::string& line, BenchmarkOrder& out) {
    // Fast path: skip blank lines and the header.
    if (line.empty() || line[0] == 'a') return false; // 'a' for "action"

    // Split on comma without regex – avoids heap allocation from std::regex.
    std::array<std::string, 5> fields;
    std::size_t field = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= line.size() && field < 5; ++i) {
        if (i == line.size() || line[i] == ',') {
            fields[field++] = line.substr(start, i - start);
            start = i + 1;
        }
    }
    if (field < 5) return false; // incomplete row

    const char action = fields[0].empty() ? '\0' : fields[0][0];
    if (action != 'A' && action != 'C') return false;

    uint64_t order_id = 0;
    int      price    = 0;
    int      qty      = 0;

    try {
        order_id = std::stoull(fields[1]);
        price    = std::stoi(fields[3]);
        qty      = std::stoi(fields[4]);
    } catch (...) {
        return false;
    }

    if (action == 'C') {
        out.is_cancel = true;
        out.cancel_id = order_id;
        // side/price/qty from a Cancel row are not used by the OrderBook
        // (cancel_order takes only the ID), so we skip them.
    } else {
        out.is_cancel      = false;
        out.order.id       = order_id;
        out.order.price    = price;
        out.order.qty      = qty;
        out.order.type     = p4::OrderType::LIMIT;
        out.order.side     = (fields[2] == "BID") ? p4::Side::BID : p4::Side::ASK;
        out.cancel_id      = 0;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// THREAD 1 – Producer ("Network / Ingest")
//
// Reads the CSV, parses each line, and spins on push() until the ring
// accepts the item.  Spinning (not sleeping) is appropriate here because
// in a real HFT environment the consumer empties the ring faster than the
// producer fills it; the ring should almost never be full.
// ─────────────────────────────────────────────────────────────────────────────
static void producer_thread(Ring& ring, const std::string& csv_path) {
    pin_thread_to_core(0); // dedicate core 0 to ingestion

    std::ifstream file(csv_path);
    if (!file.is_open()) {
        std::cerr << "[producer] Cannot open '" << csv_path << "'\n";
        producer_done.store(true, std::memory_order_release);
        return;
    }

    std::string line;
    std::size_t pushed = 0;
    std::size_t skipped = 0;

    while (std::getline(file, line)) {
        BenchmarkOrder item;
        if (!parse_csv_line(line, item)) {
            ++skipped;
            continue;
        }

        // Spin until the ring has space.
        // In production you might also check a shutdown flag here.
        while (!ring.push(item)) {
            // CPU pause hint: tells the processor we are in a spin-wait,
            // reduces power and avoids memory-order speculation penalties.
#if defined(__x86_64__) || defined(_M_X64)
            __asm__ volatile("pause" ::: "memory");
#endif
        }
        ++pushed;
    }

    std::cout << "[producer] Finished. Pushed=" << pushed
              << "  Skipped=" << skipped << "\n";

    // Signal the consumer that no more items are coming.
    // Release ordering ensures all prior ring writes are visible before
    // the consumer reads this flag.
    producer_done.store(true, std::memory_order_release);
}

// ─────────────────────────────────────────────────────────────────────────────
// THREAD 2 – Consumer ("Matching Engine")
//
// Spins on pop().  When an item arrives:
//   t0 = high_resolution_clock::now()
//   apply to OrderBook
//   t1 = high_resolution_clock::now()
//   record (t1 - t0) nanoseconds
//
// The consumer continues draining the ring even after producer_done is set,
// until the ring is confirmed empty.
// ─────────────────────────────────────────────────────────────────────────────
static std::vector<long long> consumer_thread(Ring& ring) {
    pin_thread_to_core(1); // dedicate core 1 to the matching engine

    // Pre-allocate OrderBook with known tick range and pool.
    p4::OrderBook book(PRICE_MIN, PRICE_MAX, POOL_CAP);

    std::vector<long long> latencies;
    latencies.reserve(500'000); // avoid reallocations in the hot path

    int ops_seen = 0; // total ops processed (including warm-up)

    BenchmarkOrder item;

    while (true) {
        // Attempt to pop one item.
        if (!ring.pop(item)) {
            // Ring is empty.  Check if producer is done.
            // Acquire ordering pairs with the producer's release-store.
            if (producer_done.load(std::memory_order_acquire)) {
                // Drain any remaining items that arrived between the empty
                // check and the done flag check.
                while (ring.pop(item)) {
                    // ── Hot path ─────────────────────────────────────────
                    auto t0 = std::chrono::high_resolution_clock::now();
                    try {
                        if (item.is_cancel) {
                            book.cancel_order(item.cancel_id);
                        } else {
                            book.add_order(item.order);
                        }
                    } catch (...) { /* price out of range – skip silently */ }
                    auto t1 = std::chrono::high_resolution_clock::now();
                    // ─────────────────────────────────────────────────────

                    if (++ops_seen > WARMUP_OPS) {
                        latencies.push_back(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                t1 - t0).count());
                    }
                }
                break; // truly done
            }

            // Producer still running but ring is momentarily empty – spin.
#if defined(__x86_64__) || defined(_M_X64)
            __asm__ volatile("pause" ::: "memory");
#endif
            continue;
        }

        // ── Hot path ─────────────────────────────────────────────────────
        // Timestamp is taken AFTER pop() succeeds, measuring only the
        // OrderBook operation latency (not queue-wait latency).
        auto t0 = std::chrono::high_resolution_clock::now();
        try {
            if (item.is_cancel) {
                book.cancel_order(item.cancel_id);
            } else {
                book.add_order(item.order);
            }
        } catch (...) { /* price out of range – ignore */ }
        auto t1 = std::chrono::high_resolution_clock::now();
        // ─────────────────────────────────────────────────────────────────

        if (++ops_seen > WARMUP_OPS) {
            latencies.push_back(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    t1 - t0).count());
        }
    }

    std::cout << "[consumer] Processed " << ops_seen
              << " ops total (excl. warmup recorded=" << latencies.size() << ")\n";

    // Report best/worst book state as a sanity check.
    if (!book.empty_bids())
        std::cout << "[consumer] Final best bid: " << book.best_bid() << "\n";
    if (!book.empty_asks())
        std::cout << "[consumer] Final best ask: " << book.best_ask() << "\n";

    return latencies;
}

// ─────────────────────────────────────────────────────────────────────────────
// print_stats – pretty-print latency percentiles (mirrors original benchmark)
// ─────────────────────────────────────────────────────────────────────────────
static void print_stats(const std::vector<long long>& raw, const std::string& label) {
    auto filtered = filter_outliers(raw);

    double mean = 0.0;
    if (!filtered.empty()) {
        long long sum = std::accumulate(filtered.begin(), filtered.end(), 0LL);
        mean = static_cast<double>(sum) / static_cast<double>(filtered.size());
    }

    std::cout << "\n=== Latency Report: " << label << " ===\n";
    std::cout << std::fixed << std::setprecision(1);
    std::cout << "  Total samples  : " << raw.size()       << "\n";
    std::cout << "  After filter   : " << filtered.size()  << "\n";
    std::cout << "  Mean           : " << mean                        << " ns\n";
    std::cout << "  P50            : " << percentile(filtered, 50.0)  << " ns\n";
    std::cout << "  P90            : " << percentile(filtered, 90.0)  << " ns\n";
    std::cout << "  P99            : " << percentile(filtered, 99.0)  << " ns\n";
    if (!filtered.empty()) {
        std::cout << "  Min            : " << filtered.front() << " ns\n";
        std::cout << "  Max            : " << filtered.back()  << " ns\n";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// write_csv – emit raw latencies for offline analysis
// ─────────────────────────────────────────────────────────────────────────────
static void write_csv(const std::vector<long long>& latencies,
                      const std::string& path) {
    std::ofstream f(path);
    f << "op_index,latency_ns\n";
    for (std::size_t i = 0; i < latencies.size(); ++i)
        f << i << ',' << latencies[i] << '\n';
    std::cout << "Wrote " << latencies.size() << " latency samples to '"
              << path << "'\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    // Default CSV path; override via command-line argument.
    std::string csv_path = "market_data.csv";
    if (argc > 1) csv_path = argv[1];

    std::cout << "HFT Matching Engine Pipeline\n";
    std::cout << "  CSV input      : " << csv_path      << "\n";
    std::cout << "  Ring capacity  : " << RING_CAPACITY  << " slots\n";
    std::cout << "  Warmup ops     : " << WARMUP_OPS     << "\n\n";

    // The ring buffer lives in main's stack frame; both threads hold a
    // reference to the same object (no copies, no heap allocation needed).
    Ring ring;

    // ── Launch threads ────────────────────────────────────────────────────
    // We capture `latencies` from the consumer via its return value, which
    // we retrieve through std::thread + a shared vector set before join.
    std::vector<long long> latencies;

    // Consumer is started first so it is ready to pop immediately when the
    // producer begins pushing – minimises ring-full back-pressure.
    std::thread consumer([&]() {
        latencies = consumer_thread(ring);
    });

    std::thread producer([&]() {
        producer_thread(ring, csv_path);
    });

    // Wait for both threads to finish.
    producer.join();
    consumer.join();

    // ── Post-processing ───────────────────────────────────────────────────
    if (latencies.empty()) {
        std::cerr << "[main] No latency samples collected.\n";
        return 1;
    }

    print_stats(latencies, "OrderBook operation (pop→done)");

    // Write summary CSV (same columns as original benchmark).
    {
        auto filtered = filter_outliers(latencies);
        std::ofstream summary("summary_stats_pipeline.csv");
        summary << "phase,total_samples,filtered_samples,"
                   "mean_ns,p50_ns,p90_ns,p99_ns,min_ns,max_ns\n";
        double mean = filtered.empty() ? 0.0 :
            static_cast<double>(
                std::accumulate(filtered.begin(), filtered.end(), 0LL)) /
            static_cast<double>(filtered.size());
        summary << "spsc_pipeline,"
                << latencies.size()   << ','
                << filtered.size()    << ','
                << mean               << ','
                << percentile(filtered, 50.0) << ','
                << percentile(filtered, 90.0) << ','
                << percentile(filtered, 99.0) << ','
                << (filtered.empty() ? 0 : filtered.front()) << ','
                << (filtered.empty() ? 0 : filtered.back())  << '\n';
    }

    write_csv(latencies, "raw_latencies_pipeline.csv");

    std::cout << "\nDone.\n";
    return 0;
}