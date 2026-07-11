#include <iostream>
#include <sstream>
#include <queue>
#include <vector>
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

    uint64_t next_id = 1;
    char buffer[1024];
    string leftover;

    while (true) {
        memset(buffer, 0, sizeof(buffer));
        int bytes = read(client_socket, buffer, sizeof(buffer));
        if (bytes <= 0) break;

        leftover += string(buffer, bytes);

        // parse all complete messages in buffer
        size_t pos;
        while ((pos = leftover.find('\n')) != string::npos) {
            string message = leftover.substr(0, pos);
            leftover = leftover.substr(pos + 1);

            stringstream ss(message);
            string side_str;
            int price, qty;
            ss >> side_str >> price >> qty;

            Side side = (side_str[0] == 'B') ? Side::Bid : Side::Ask;
            Order order{ price, qty, next_id++, side };
            ob.add_order(order);

            // send ack
            uint8_t ack = 1;
            write(client_socket, &ack, 1);
        }
    }

    close(client_socket);
    close(server_fd);
    return 0;
}