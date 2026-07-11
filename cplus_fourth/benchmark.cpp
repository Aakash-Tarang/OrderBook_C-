
#include "orderbook.h"
#include <chrono>
#include <random>
#include <vector>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <numeric>
#include <string>



struct BenchmarkOrder {
    p4::Order order;
    bool      is_cancel = false;
    uint64_t  cancel_id = 0;
};

static std::vector<BenchmarkOrder> generate_dense_workload(
        int n_orders, double cancel_ratio, int price_min, int price_max, uint64_t seed)
{
    std::mt19937_64                    rng(seed);
    std::uniform_int_distribution<int> price_dist(price_min, price_max);
    std::uniform_int_distribution<int> qty_dist(1, 100);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_real_distribution<>   action_dist(0.0, 1.0);

    std::vector<BenchmarkOrder> workload;
    workload.reserve(n_orders);

    std::vector<uint64_t> live_ids;
    uint64_t next_id = 1;

    for (int i = 0; i < n_orders; ++i) {
        BenchmarkOrder bw;
        if (!live_ids.empty() && action_dist(rng) < cancel_ratio) {
            std::uniform_int_distribution<size_t> idx_dist(0, live_ids.size() - 1);
            size_t idx   = idx_dist(rng);
            bw.is_cancel = true;
            bw.cancel_id = live_ids[idx];
            live_ids.erase(live_ids.begin() + static_cast<std::ptrdiff_t>(idx));
        } else {
            bw.is_cancel   = false;
            bw.order.id    = next_id++;
            bw.order.side  = (side_dist(rng) == 0) ? p4::Side::BID : p4::Side::ASK;
            bw.order.price = price_dist(rng);
            bw.order.qty   = qty_dist(rng);
            bw.order.type  = p4::OrderType::LIMIT;
            live_ids.push_back(bw.order.id);
        }
        workload.push_back(bw);
    }
    return workload;
}

static std::vector<BenchmarkOrder> generate_sparse_workload(
        int n_orders, double cancel_ratio, int price_min, int price_max,
        int n_clusters, int cluster_jitter, uint64_t seed)
{
    std::mt19937_64                    rng(seed);
    std::uniform_int_distribution<int> cluster_dist(0, n_clusters - 1);
    std::uniform_int_distribution<int> jitter_dist(-cluster_jitter, cluster_jitter);
    std::uniform_int_distribution<int> qty_dist(1, 100);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_real_distribution<>   action_dist(0.0, 1.0);

    std::vector<int> cluster_centers(n_clusters);
    {
        long long span = static_cast<long long>(price_max - price_min);
        for (int c = 0; c < n_clusters; ++c)
            cluster_centers[c] = price_min +
                static_cast<int>(span * (c + 0.5) / n_clusters);
    }

    std::vector<BenchmarkOrder> workload;
    workload.reserve(n_orders);

    std::vector<uint64_t> live_ids;
    uint64_t next_id = 1;

    for (int i = 0; i < n_orders; ++i) {
        BenchmarkOrder bw;
        if (!live_ids.empty() && action_dist(rng) < cancel_ratio) {
            std::uniform_int_distribution<size_t> idx_dist(0, live_ids.size() - 1);
            size_t idx   = idx_dist(rng);
            bw.is_cancel = true;
            bw.cancel_id = live_ids[idx];
            live_ids.erase(live_ids.begin() + static_cast<std::ptrdiff_t>(idx));
        } else {
            int center = cluster_centers[cluster_dist(rng)];
            int price  = std::clamp(center + jitter_dist(rng), price_min, price_max);

            bw.is_cancel   = false;
            bw.order.id    = next_id++;
            bw.order.side  = (side_dist(rng) == 0) ? p4::Side::BID : p4::Side::ASK;
            bw.order.price = price;
            bw.order.qty   = qty_dist(rng);
            bw.order.type  = p4::OrderType::LIMIT;
            live_ids.push_back(bw.order.id);
        }
        workload.push_back(bw);
    }
    return workload;
}

static double percentile(std::vector<long long> v, double p)
{
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    double idx = p / 100.0 * (static_cast<double>(v.size()) - 1.0);
    auto   lo   = static_cast<size_t>(idx);
    double frac = idx - static_cast<double>(lo);
    if (lo + 1 >= v.size()) return static_cast<double>(v.back());
    return static_cast<double>(v[lo]) * (1.0 - frac) +
           static_cast<double>(v[lo + 1]) * frac;
}

static std::vector<long long> filter_outliers(std::vector<long long> v, double fence = 3.0)
{
    if (v.empty()) return v;
    std::sort(v.begin(), v.end());
    double q1 = percentile(v, 25.0);
    double q3 = percentile(v, 75.0);
    double iqr = q3 - q1;
    long long lo = static_cast<long long>(q1 - fence * iqr);
    long long hi = static_cast<long long>(q3 + fence * iqr);
    std::vector<long long> out;
    out.reserve(v.size());
    for (auto x : v) if (x >= lo && x <= hi) out.push_back(x);
    return out;
}

struct RunResult {
    std::string             phase;
    std::string             workload;
    std::vector<long long>  samples_ns;
    std::vector<char>       op_type;
};

static void write_summary_row(std::ofstream& out, const RunResult& r)
{
    auto filtered = filter_outliers(r.samples_ns);
    double mean = filtered.empty() ? 0.0 :
        std::accumulate(filtered.begin(), filtered.end(), 0LL) /
        static_cast<double>(filtered.size());

    out << r.phase << ',' << r.workload << ','
        << r.samples_ns.size() << ',' << filtered.size() << ','
        << mean << ','
        << percentile(filtered, 50.0) << ','
        << percentile(filtered, 90.0) << ','
        << percentile(filtered, 99.0) << ','
        << (filtered.empty() ? 0 : filtered.front()) << ','
        << (filtered.empty() ? 0 : filtered.back()) << '\n';
}

static void print_stats(const RunResult& r)
{
    auto filtered = filter_outliers(r.samples_ns);
    double mean = filtered.empty() ? 0.0 :
        std::accumulate(filtered.begin(), filtered.end(), 0LL) /
        static_cast<double>(filtered.size());

    std::cout << "\n=== " << r.phase << " [" << r.workload << "] ===\n";
    std::cout << "  Total samples : " << r.samples_ns.size() << "\n";
    std::cout << "  After filter  : " << filtered.size()     << "\n";
    std::cout << "  Mean          : " << mean                       << " ns\n";
    std::cout << "  P50           : " << percentile(filtered, 50.0) << " ns\n";
    std::cout << "  P90           : " << percentile(filtered, 90.0) << " ns\n";
    std::cout << "  P99           : " << percentile(filtered, 99.0) << " ns\n";
    if (!filtered.empty()) {
        std::cout << "  Min           : " << filtered.front() << " ns\n";
        std::cout << "  Max           : " << filtered.back()  << " ns\n";
    }
}

static RunResult run_phase4(const std::vector<BenchmarkOrder>& wl, int warmup,
                             const std::string& workload_name, int price_min, int price_max)
{
    // Allocate pool size slightly larger than the total workload to be completely safe
    size_t pool_capacity = wl.size() + 1000;
    p4::OrderBook book(price_min, price_max, pool_capacity);
    
    RunResult r;
    r.phase = "phase4_bitmask_pool";
    r.workload = workload_name;
    r.samples_ns.reserve(wl.size() - warmup);
    r.op_type.reserve(wl.size() - warmup);

    for (int i = 0; i < static_cast<int>(wl.size()); ++i) {
        const BenchmarkOrder& bw = wl[i];

        auto t0 = std::chrono::high_resolution_clock::now();
        if (bw.is_cancel) {
            book.cancel_order(bw.cancel_id);
        } else {
            book.add_order(bw.order);
        }
        auto t1 = std::chrono::high_resolution_clock::now();

        if (i >= warmup) {
            r.samples_ns.push_back(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            r.op_type.push_back(bw.is_cancel ? 'C' : 'A');
        }
    }
    return r;
}

int main()
{
    
    constexpr int    DENSE_N      = 300'000;
    constexpr int    DENSE_WARMUP = 5'000;
    constexpr double CANCEL_RATIO = 0.40;
    constexpr int    DENSE_MIN    = 9'900;
    constexpr int    DENSE_MAX    = 10'100;
    constexpr int    SPARSE_N       = 150'000;
    constexpr int    SPARSE_WARMUP  = 2'000;
    constexpr int    SPARSE_MIN     = 0;
    constexpr int    SPARSE_MAX     = 200'000;
    constexpr int    SPARSE_CLUSTERS = 20;
    constexpr int    SPARSE_JITTER   = 3;

    std::cout << "Generating dense workload (" << DENSE_N << " ops)...\n";
    auto dense_wl = generate_dense_workload(DENSE_N, CANCEL_RATIO, DENSE_MIN, DENSE_MAX, 42);

    std::cout << "Generating sparse workload (" << SPARSE_N << " ops)...\n";
    auto sparse_wl = generate_sparse_workload(SPARSE_N, CANCEL_RATIO, SPARSE_MIN, SPARSE_MAX,
                                               SPARSE_CLUSTERS, SPARSE_JITTER, 43);

    std::vector<RunResult> results;
    

    results.push_back(run_phase4(dense_wl, DENSE_WARMUP, "dense", DENSE_MIN, DENSE_MAX));
    results.push_back(run_phase4(sparse_wl, SPARSE_WARMUP, "sparse", SPARSE_MIN, SPARSE_MAX));

    for (const auto& r : results) print_stats(r);

    
    {
       
        std::ofstream summary("summary_stats_phase4.csv");
        summary << "phase,workload,total_samples,filtered_samples,"
                   "mean_ns,p50_ns,p90_ns,p99_ns,min_ns,max_ns\n";
        for (const auto& r : results) write_summary_row(summary, r);
    }

    {
        std::ofstream raw("raw_latencies_phase4.csv");
        raw << "phase,workload,op_index,op_type,latency_ns\n";
        for (const auto& r : results) {
            for (size_t i = 0; i < r.samples_ns.size(); ++i) {
                raw << r.phase << ',' << r.workload << ',' << i << ','
                    << (r.op_type[i] == 'A' ? "add" : "cancel") << ','
                    << r.samples_ns[i] << '\n';
            }
        }
    }

    std::cout << "\nWrote summary_stats_phase4.csv and raw_latencies_phase4.csv\n";
    return 0;
}