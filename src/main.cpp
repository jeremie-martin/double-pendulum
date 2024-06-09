#include "draw.h"
#include "simulation.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

double deg2rad(double degrees) { return degrees * M_PI / 180.0; }

double angle_around_at_index(double center_angle, double variation_angle, double N, double index) {
    double step_size = variation_angle / (N - 1);
    double start_angle = center_angle - variation_angle / 2;
    return start_angle + index * step_size;
}

void create_pendulums(
    int start,
    int end,
    double G,
    double L1,
    double L2,
    double M1,
    double M2,
    double th2,
    double w1,
    double w2,
    double pendulum_nb,
    std::vector<Pendulum>& pendulums
) {
    for (int i = start; i < end; ++i) {
        double th1 = angle_around_at_index(deg2rad(-0.1), deg2rad(1), pendulum_nb, i);
        pendulums[i] = Pendulum(G, L1, L2, M1, M2, th1, th2, w1, w2);
    }
}

int main() {
    double G = 9.81;
    double L1 = -1.0;
    double L2 = -1.0;
    double M1 = 1.0;
    double M2 = 1.0;
    double th1 = deg2rad(-0.1);
    double th2 = deg2rad(-0.001);
    double w1 = 0.0;
    double w2 = 0.0;

    int pendulum_nb = 1e7;
    std::vector<Pendulum> pendulums(pendulum_nb); // Pre-allocate memory

    auto start_time = std::chrono::high_resolution_clock::now();

    int thread_count = std::thread::hardware_concurrency();
    std::vector<std::thread> threads;
    int chunk_size = pendulum_nb / thread_count;

    for (int i = 0; i < thread_count; ++i) {
        int start = i * chunk_size;
        int end = (i == thread_count - 1) ? pendulum_nb : start + chunk_size;
        threads.emplace_back(create_pendulums, start, end, G, L1, L2, M1, M2, th2, w1, w2, pendulum_nb, std::ref(pendulums));
    }

    for (auto& thread : threads) {
        thread.join();
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
    std::cout << "Time taken (ms): " << duration << std::endl;

    return 0;
}
