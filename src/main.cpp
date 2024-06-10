#include "draw.h"
#include "pendulum.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

double deg2rad(double degrees) { return degrees * M_PI / 180.0; }

double angle_around_at_index(double center_angle, double variation_angle, double N, double index) {
    double step_size = variation_angle / (N - 1);
    double start_angle = center_angle - variation_angle / 2;
    return start_angle + index * step_size;
}

double rad2deg(double radians) { return radians * 180.0 / M_PI; }

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
        double th1 = angle_around_at_index(deg2rad(10), deg2rad(1), pendulum_nb, i);
        pendulums[i] = Pendulum(G, L1, L2, M1, M2, th1, th2, w1, w2);
    }
    exit(0);
}

void compute_steps(int start, int end, int steps_count, double dt, std::vector<Pendulum>& pendulums, std::vector<std::vector<PendulumState>>& states_all) {
    for (int i = start; i < end; ++i) {
        states_all[i] = pendulums[i].steps(steps_count, dt);
    }
}

void process_frames(
    int start,
    int end,
    std::vector<std::vector<PendulumState>> const& states_all,
    int pendulum_nb,
    int width,
    int height,
    int centerX,
    int centerY,
    std::mutex& print_mutex
) {
    for (int i = start; i < end; ++i) {
        Image image(width, height);

        for (int j = 0; j < pendulum_nb; ++j) {
            PendulumState const& state = states_all[j][i];
            int x0 = centerX;
            int y0 = centerY;
            int x1 = static_cast<int>(centerX + state.x1 * width / 5);
            int y1 = static_cast<int>(centerY + state.y1 * width / 5);
            int x2 = static_cast<int>(centerX + state.x2 * width / 5);
            int y2 = static_cast<int>(centerY + state.y2 * width / 5);

            float freq = 380 + j * (780 - 380) / double(pendulum_nb);
            Color color = wavelengthToRGB(freq);
            image.draw_line(x0, y0, x1, y1, color);
            image.draw_line(x1, y1, x2, y2, color);
        }

        auto min_max = std::minmax_element(image.data.begin(), image.data.end());
        auto min = *min_max.first;
        auto max = *min_max.second;
        float factor = 0.092;

        for (int x = 0; x < width; ++x) {
            for (int y = 0; y < height; ++y) {
                Color color = image.get_pixel(x, y);

                color.r = (color.r - min) / (max - min);
                color.g = (color.g - min) / (max - min);
                color.b = (color.b - min) / (max - min);

                color.r = std::pow(color.r, 1.0 / 1.05) * 255.0 * factor * std::min(width, height);
                color.g = std::pow(color.g, 1.0 / 1.05) * 255.0 * factor * std::min(width, height);
                color.b = std::pow(color.b, 1.0 / 1.05) * 255.0 * factor * std::min(width, height);

                image.set_pixel(x, y, color);
            }
        }

        std::ostringstream file_path;
        file_path << "data/img" << std::setfill('0') << std::setw(4) << i << ".png";
        image.save_to_png(file_path.str().c_str());

        {
            std::lock_guard<std::mutex> lock(print_mutex);
            printf("Saved image %03d\r", i);
            fflush(stdout);
        }
    }
}

int main() {
    double G = 9.81;
    double L1 = -1.0;
    double L2 = -1.0;
    double M1 = 1.0;
    double M2 = 1.0;
    double th2 = deg2rad(-0.001);
    double w1 = 0.0;
    double w2 = 0.0;
    double t_stop = 1;
    double FPS = 60;
    double dt = t_stop / (2.0 * FPS);
    int steps_count = static_cast<int>(t_stop / dt);

    int pendulum_nb = 1e5;
    std::vector<Pendulum> pendulums(pendulum_nb);

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

    std::vector<std::vector<PendulumState>> states_all(pendulum_nb);

    threads.clear();
    for (int i = 0; i < thread_count; ++i) {
        int start = i * chunk_size;
        int end = (i == thread_count - 1) ? pendulum_nb : start + chunk_size;
        threads.emplace_back(compute_steps, start, end, steps_count, dt, std::ref(pendulums), std::ref(states_all));
    }

    for (auto& thread : threads) {
        thread.join();
    }

    int const width = 1080;
    int const height = 1080;
    int const centerX = width / 2;
    int const centerY = height / 2;

    int N = 32;
    int total_frames = states_all[0].size();
    int frames_per_thread = total_frames / N;

    threads.clear();
    std::mutex print_mutex;

    auto start_time = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < N; ++i) {
        int start = i * frames_per_thread;
        int end = (i == N - 1) ? total_frames : (i + 1) * frames_per_thread;
        threads.emplace_back(process_frames, start, end, std::ref(states_all), pendulum_nb, width, height, centerX, centerY, std::ref(print_mutex));
    }

    for (auto& thread : threads) {
        thread.join();
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
    std::cout << "Time taken (ms): " << duration << std::endl;

    std::cout << "Simulation complete. Images saved to 'data/' directory." << std::endl;

    return 0;
}
