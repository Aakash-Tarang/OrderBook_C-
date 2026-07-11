#include <iostream>
#include <random>
#include <chrono>
#include <vector>
#include <fstream>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <unistd.h>

using namespace std;

#pragma pack(push,1)

struct OrderMessage {
    uint8_t type;
    uint8_t side;
    int32_t price;
    int32_t qty;
    uint64_t order_id;
};

#pragma pack(pop)

int main() {

    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (sock_fd < 0) {
        perror("socket");
        return 1;
    }

    int flag = 1;

    setsockopt(
        sock_fd,
        IPPROTO_TCP,
        TCP_NODELAY,
        &flag,
        sizeof(flag)
    );

    sockaddr_in server_addr{};

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(8080);

    inet_pton(
        AF_INET,
        "127.0.0.1",
        &server_addr.sin_addr
    );

    if (connect(
            sock_fd,
            (sockaddr*)&server_addr,
            sizeof(server_addr)
        ) < 0) {

        perror("connect");
        return 1;
    }

    random_device rd;
    mt19937 rng(rd());

    uniform_int_distribution<int> side_dist(0, 1);
    uniform_int_distribution<int> price_dist(95, 105);
    uniform_int_distribution<int> qty_dist(1, 10);

    const int iterations = 100000;

    ofstream csv("latency.csv");

    csv << "iteration,latency_us\n";

    for (uint64_t i = 1; i <= iterations; i++) {

        OrderMessage msg;

        msg.type = 0;
        msg.side = side_dist(rng);

        msg.price = price_dist(rng);
        msg.qty = qty_dist(rng);

        msg.order_id = i;

        auto t1 =
            chrono::high_resolution_clock::now();

        size_t total_sent = 0;

        while (total_sent < sizeof(OrderMessage)) {

            ssize_t sent = send(
                sock_fd,
                ((char*)&msg) + total_sent,
                sizeof(OrderMessage) - total_sent,
                0
            );

            if (sent <= 0) {
                perror("send");
                return 1;
            }

            total_sent += sent;
        }

        uint8_t ack;

        size_t total_received = 0;

        while (total_received < sizeof(ack)) {

            ssize_t received = read(
                sock_fd,
                ((char*)&ack) + total_received,
                sizeof(ack) - total_received
            );

            if (received <= 0) {
                perror("read");
                return 1;
            }

            total_received += received;
        }

        auto t2 =
            chrono::high_resolution_clock::now();

        long long latency =
            chrono::duration_cast<
                chrono::microseconds
            >(t2 - t1).count();

        csv << i << ","
            << latency
            << "\n";
    }

    cout << "Latency data saved to latency.csv\n";

    close(sock_fd);

    return 0;
}