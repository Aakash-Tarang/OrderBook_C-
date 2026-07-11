#include <iostream>
#include <string>
#include <random>
#include <chrono>
#include <climits>
#include <vector>
#include <fstream>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <unistd.h>

using namespace std;

int main() {

    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (sock_fd < 0) {
        perror("socket");
        return 1;
    }

    int flag = 1;
    setsockopt(sock_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(8080);

    int rc = inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);

    if (rc == 0) {
        fprintf(stderr, "invalid address\n");
        return 1;
    }

    if (rc < 0) {
        perror("inet_pton");
        return 1;
    }

    if (connect(sock_fd,
                (sockaddr*)&server_addr,
                sizeof(server_addr)) < 0) {

        perror("connect");
        return 1;
    }

    random_device rd;
    mt19937 rng(rd());

    uniform_int_distribution<int> side_dist(0, 1);
    uniform_int_distribution<int> price_dist(95, 105);
    uniform_int_distribution<int> qty_dist(1, 10);

    const int iterations = 100000;

    long long min_latency = LLONG_MAX;
    long long max_latency = LLONG_MIN;
    long long total_latency = 0;

    vector<long long> latencies;
    latencies.reserve(iterations);

    for (int i = 0; i < iterations; i++) {

        string side = side_dist(rng) ? "BID" : "ASK";

        int price = price_dist(rng);
        int qty = qty_dist(rng);

        string msg =
            side + " " +
            to_string(price) + " " +
            to_string(qty) + "\n";

        auto t1 = chrono::high_resolution_clock::now();

        // complete send loop
        size_t total_sent = 0;

        while (total_sent < msg.size()) {

            ssize_t sent = send(
                sock_fd,
                msg.c_str() + total_sent,
                msg.size() - total_sent,
                0
            );

            if (sent <= 0) {
                perror("send");
                close(sock_fd);
                return 1;
            }

            total_sent += sent;
        }

        uint8_t ack = 0;

        ssize_t received = read(sock_fd, &ack, 1);

        if (received <= 0) {
            perror("read");
            close(sock_fd);
            return 1;
        }

        auto t2 = chrono::high_resolution_clock::now();

        long long latency =
            chrono::duration_cast<chrono::microseconds>(t2 - t1).count();

        latencies.push_back(latency);

        total_latency += latency;

        if (latency < min_latency)
            min_latency = latency;

        if (latency > max_latency)
            max_latency = latency;
    }

    // dump CSV AFTER benchmark
    ofstream csv("latency.csv");

    csv << "iteration,latency_us\n";

    for (size_t i = 0; i < latencies.size(); i++) {
        csv << i << "," << latencies[i] << "\n";
    }

    csv.close();

    cout << "\n--- BENCHMARK RESULTS (" << iterations << " orders) ---\n";

    cout << "MIN:  " << min_latency << " us\n";
    cout << "MAX:  " << max_latency << " us\n";
    cout << "MEAN: " << total_latency / iterations << " us\n";

    cout << "\nSaved latency data to latency.csv\n";

    close(sock_fd);

    return 0;
}