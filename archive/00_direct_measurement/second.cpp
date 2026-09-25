#include <iostream>
#include <vector>
#include <queue>
#include <unordered_map>
#include <cstdint>
#include <climits>
#include <sstream>
#include <algorithm>
#include <arpa/inet.h>
#include <unistd.h>
#include <chrono>
#include <fstream>
#include <random>
using namespace std;


enum class Side : uint8_t { Bid, Ask };

struct Order {
    int price;
    int qty;
    uint64_t id;
    Side side;
    bool cancelled;
};


#pragma pack(push,1)

struct OrderMessage {
    uint8_t type;
    uint8_t side;
    int32_t price;
    int32_t qty;
    uint64_t order_id;
};

#pragma pack(pop)

struct PriceLevel {
    queue<uint64_t> orders; // stores orderIDs
    int total_qty = 0;
};

struct OrderBook {
    static const int MIN_PRICE = 0;
    static const int MAX_PRICE = 10000;
    static const int TICK_SIZE = 5;

    vector<PriceLevel> asks;
    vector<PriceLevel> bids;
    unordered_map<uint64_t, Order> order_map;

    int best_bid = INT_MIN;
    int best_ask = INT_MAX;

    OrderBook(): asks((MAX_PRICE - MIN_PRICE) / TICK_SIZE + 1),bids((MAX_PRICE - MIN_PRICE) / TICK_SIZE + 1) {}

    int to_index(int raw_price) {
        if (raw_price > MAX_PRICE || raw_price < MIN_PRICE) return -1;
        return (raw_price - MIN_PRICE) / TICK_SIZE;
    }

    void add_order(Order order) {
        int idx = to_index(order.price);
        if (idx == -1) return;  // silent failure 

        order.cancelled = false;
        order_map[order.id] = order; 

        if (order.side == Side::Bid) {
            bids[idx].orders.push(order.id);  
            bids[idx].total_qty += order.qty;
            if (order.price > best_bid) best_bid = order.price;
        } else {
            asks[idx].orders.push(order.id);  
            asks[idx].total_qty += order.qty;
            if (order.price < best_ask) best_ask = order.price;
        }

        match();
    }

    void cancel_order(uint64_t id) {
        auto it = order_map.find(id);
        if (it == order_map.end()) return;

        Order& order = it->second;
        order.cancelled = true;

        int idx = to_index(order.price);
        if (idx == -1) return;

        if (order.side == Side::Bid) {
            bids[idx].total_qty -= order.qty;
        } else {
            asks[idx].total_qty -= order.qty;
        }
    }

    void match() {
        while (best_bid >= best_ask) {
            int bid_idx = to_index(best_bid);
            int ask_idx = to_index(best_ask);

            PriceLevel& bid_level = bids[bid_idx];
            PriceLevel& ask_level = asks[ask_idx];


            while (!bid_level.orders.empty()) {
                auto it = order_map.find(bid_level.orders.front());
                if (it != order_map.end() && !it->second.cancelled) break;
                if (it != order_map.end()) order_map.erase(it);
                bid_level.orders.pop();
            }

            while (!ask_level.orders.empty()) {
                auto it = order_map.find(ask_level.orders.front());
                if (it != order_map.end() && !it->second.cancelled) break;
                if (it != order_map.end()) order_map.erase(it);
                ask_level.orders.pop();
            }

            if (bid_level.orders.empty()) {
                best_bid = find_next_bid(bid_idx - 1);
                break;
            }
            if (ask_level.orders.empty()) {
                best_ask = find_next_ask(ask_idx + 1);
                break;
            }

            Order& bid = order_map[bid_level.orders.front()];
            Order& ask = order_map[ask_level.orders.front()];

            int traded_qty = min(bid.qty, ask.qty);

            bid.qty -= traded_qty;
            ask.qty -= traded_qty;

            bid_level.total_qty -= traded_qty;
            ask_level.total_qty -= traded_qty;

            // remove fully filled orders
            if (bid.qty == 0) {
                order_map.erase(bid.id);
                bid_level.orders.pop();
            }
            if (ask.qty == 0) {
                order_map.erase(ask.id);
                ask_level.orders.pop();
            }

            // update best prices if levels exhausted
            if (bid_level.orders.empty()) {
                best_bid = find_next_bid(bid_idx - 1);
            }
            if (ask_level.orders.empty()) {
                best_ask = find_next_ask(ask_idx + 1);
            }
        }
    }

    // scan down from idx to find next occupied bid level
    int find_next_bid(int from_idx) {
        for (int i = from_idx; i >= 0; i--) {
            if (bids[i].total_qty > 0) return i * TICK_SIZE + MIN_PRICE;  
        }
        return INT_MIN;
    }

    // scan up from idx to find next occupied ask level
    int find_next_ask(int from_idx) {
        for (int i = from_idx; i <= (MAX_PRICE - MIN_PRICE) / TICK_SIZE; i++) {
            if (asks[i].total_qty > 0) return i * TICK_SIZE + MIN_PRICE;  
        }
        return INT_MAX;
    }
};

// ─── in-process benchmark ─────────────────────────────────────────────────────

void run_benchmark() {
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
        o.price     = price_dist(rng);
        o.qty       = qty_dist(rng);
        o.id        = i;
        o.side      = side_dist(rng) ? Side::Bid : Side::Ask;
        o.cancelled = false;
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

    // compute stats
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
    cout << "Samples : " << iterations                  << "\n";
    cout << "MIN     : " << mn                          << " ns\n";
    cout << "MAX     : " << mx                          << " ns\n";
    cout << "MEAN    : " << total / iterations          << " ns\n";
    cout << "P50     : " << p50                         << " ns\n";
    cout << "P90     : " << p90                         << " ns\n";
    cout << "P99     : " << p99                         << " ns\n";

    ofstream csv("latency_inprocess.csv");
    csv << "iteration,latency_ns\n";
    for (int i = 0; i < iterations; i++)
        csv << i << "," << latencies[i] << "\n";
    csv.close();
    cout << "Saved to latency_inprocess.csv\n";
}

// ─── network server (unchanged behaviour) ─────────────────────────────────────

void run_server() {
    OrderBook ob;
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in server_addr{};
    server_addr.sin_family      = AF_INET;
    server_addr.sin_port        = htons(8080);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (::bind(server_fd, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind"); return;
    }
    if (listen(server_fd, 5) < 0) { perror("listen"); return; }

    cout << "OrderBook listening on port 8080\n";

    int client_socket = accept(server_fd, nullptr, nullptr);
    if (client_socket < 0) { perror("accept"); return; }
    cout << "Client connected\n";

    while (true) {
        OrderMessage msg;
        size_t total_read = 0;
        while (total_read < sizeof(OrderMessage)) {
            ssize_t bytes = read(
                client_socket,
                ((char*)&msg) + total_read,
                sizeof(OrderMessage) - total_read
            );
            if (bytes <= 0) {
                close(client_socket);
                close(server_fd);
                return;
            }
            total_read += bytes;
        }

        if (msg.type == 0) {
            Order order;
            order.price = msg.price;
            order.qty   = msg.qty;
            order.id    = msg.order_id;
            order.side  = (msg.side == 0) ? Side::Bid : Side::Ask;
            ob.add_order(order);
        } else if (msg.type == 1) {
            ob.cancel_order(msg.order_id);
        }

        uint8_t ack = 1;
        write(client_socket, &ack, 1);
    }

    close(client_socket);
    close(server_fd);
}

int main(int argc, char* argv[]) {
    // run as: ./server --benchmark   for in-process measurement
    //         ./server               for normal network mode
    if (argc > 1 && string(argv[1]) == "--benchmark") {
        run_benchmark();
    } else {
        run_server();
    }
    return 0;
}