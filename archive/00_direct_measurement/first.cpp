#include <iostream>
#include <queue>
#include <vector>
#include <algorithm>
#include <random>
#include <chrono>
#include <fstream>
#include <climits>
using namespace std;

enum class Side : uint8_t { Bid, Ask };

struct Order {
    int price;
    int qty;
    uint64_t id;
    Side side;
};

struct CompareBid {
    bool operator()(const Order& a, const Order& b) {
        if (a.price < b.price) return true;
        if (a.price == b.price) return a.id > b.id;
        return false;
    }
};

struct CompareAsk {
    bool operator()(const Order& a, const Order& b) {
        if (a.price > b.price) return true;
        if (a.price == b.price) return a.id > b.id;
        return false;
    }
};

struct OrderBook {
    priority_queue<Order, vector<Order>, CompareBid> bids;
    priority_queue<Order, vector<Order>, CompareAsk> asks;

    void match() {
        while (!bids.empty() && !asks.empty() &&
               bids.top().price >= asks.top().price) {
            Order bid = bids.top();
            Order ask = asks.top();
            bids.pop();
            asks.pop();
            int traded_qty = min(bid.qty, ask.qty);
            bid.qty -= traded_qty;
            ask.qty -= traded_qty;
            if (bid.qty > 0) bids.push(bid);
            if (ask.qty > 0) asks.push(ask);
        }
    }

    void add_order(Order order) {
        if (order.side == Side::Bid) bids.push(order);
        else asks.push(order);
        match();
    }
};

int main() {
    const int iterations = 100000;

    random_device rd;
    mt19937 rng(rd());
    uniform_int_distribution<int> side_dist(0, 1);
    uniform_int_distribution<int> price_dist(95, 105);
    uniform_int_distribution<int> qty_dist(1, 10);

    // pre-generate all orders so RNG cost is excluded from timing
    vector<Order> orders;
    orders.reserve(iterations);
    for (uint64_t i = 1; i <= iterations; i++) {
        Order o;
        o.price = price_dist(rng);
        o.qty   = qty_dist(rng);
        o.id    = i;
        o.side  = side_dist(rng) ? Side::Bid : Side::Ask;
        orders.push_back(o);
    }

    OrderBook ob;
    vector<long long> latencies;
    latencies.reserve(iterations);

    for (int i = 0; i < iterations; i++) {
        auto t1 = chrono::high_resolution_clock::now();
        ob.add_order(orders[i]);
        auto t2 = chrono::high_resolution_clock::now();
        latencies.push_back(
            chrono::duration_cast<chrono::nanoseconds>(t2 - t1).count()
        );
    }

    long long total = 0, mn = LLONG_MAX, mx = LLONG_MIN;
    for (auto l : latencies) {
        total += l;
        if (l < mn) mn = l;
        if (l > mx) mx = l;
    }

    vector<long long> sorted = latencies;
    sort(sorted.begin(), sorted.end());
    long long p50 = sorted[iterations * 50 / 100];
    long long p90 = sorted[iterations * 90 / 100];
    long long p99 = sorted[iterations * 99 / 100];

    cout << "\n========== IN-PROCESS BENCHMARK (nanoseconds) ==========\n";
    cout << "Samples : " << iterations         << "\n";
    cout << "MIN     : " << mn                 << " ns\n";
    cout << "MAX     : " << mx                 << " ns\n";
    cout << "MEAN    : " << total / iterations << " ns\n";
    cout << "P50     : " << p50                << " ns\n";
    cout << "P90     : " << p90                << " ns\n";
    cout << "P99     : " << p99                << " ns\n";

    ofstream csv("latency_inprocess_pq.csv");
    csv << "iteration,latency_ns\n";
    for (int i = 0; i < iterations; i++)
        csv << i << "," << latencies[i] << "\n";
    csv.close();
    cout << "Saved to latency_inprocess_pq.csv\n";

    return 0;
}