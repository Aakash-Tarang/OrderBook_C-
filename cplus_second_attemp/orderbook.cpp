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

// main_function

int main(){
    OrderBook ob;
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(8080);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (::bind(server_fd, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        return 1;
    }

    if (listen(server_fd, 5) < 0) {
        perror("listen");
        return 1;
    }

    cout << "OrderBook listening on port 8080\n";

    int client_socket = accept(server_fd, nullptr, nullptr);
    if (client_socket < 0) { perror("accept"); return 1; }
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
                return 0;
            }

            total_read += bytes;
        }

        // process message

        if (msg.type == 0) {

            Order order;

            order.price = msg.price;
            order.qty = msg.qty;
            order.id = msg.order_id;

            order.side =
                (msg.side == 0)
                ? Side::Bid
                : Side::Ask;

            ob.add_order(order);
        }

        else if (msg.type == 1) {

            ob.cancel_order(msg.order_id);
        }

        // ACK
        uint8_t ack = 1;

        write(client_socket, &ack, 1);
    }

    // close communication 
    close(client_socket);
    close(server_fd);
    return 0;
}