#include "draw.h"
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

struct PendulumState
{
    double x1, y1, x2, y2;
};

std::vector<PendulumState> simulate_double_pendulum(
    double G,
    double L1,
    double L2,
    double M1,
    double M2,
    double th1,
    double th2,
    double w1,
    double w2,
    double t_stop,
    double dt
) {
    std::vector<PendulumState> states;
    double t = 0;
    while (t < t_stop) {
        double num1 = -G * (2 * M1 + M2) * sin(th1);
        double num2 = -M2 * G * sin(th1 - 2 * th2);
        double num3 = -2 * sin(th1 - th2) * M2;
        double num4 = w2 * w2 * L2 + w1 * w1 * L1 * cos(th1 - th2);
        double den = L1 * (2 * M1 + M2 - M2 * cos(2 * th1 - 2 * th2));
        double a1 = (num1 + num2 + num3 * num4) / den;

        num1 = 2 * sin(th1 - th2);
        num2 = (w1 * w1 * L1 * (M1 + M2));
        num3 = G * (M1 + M2) * cos(th1);
        num4 = w2 * w2 * L2 * M2 * cos(th1 - th2);
        den = L2 * (2 * M1 + M2 - M2 * cos(2 * th1 - 2 * th2));
        double a2 = (num1 * (num2 + num3 + num4)) / den;

        th1 += w1 * dt;
        th2 += w2 * dt;
        w1 += a1 * dt;
        w2 += a2 * dt;

        double x1 = L1 * sin(th1);
        double y1 = L1 * cos(th1);
        double x2 = x1 + L2 * sin(th2);
        double y2 = y1 + L2 * cos(th2);

        states.push_back({ x1, y1, x2, y2 });
        t += dt;
    }
    return states;
}

double deg2rad(double degrees) { return degrees * M_PI / 180.0; }

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
            PendulumState state = states_all[j][i];
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

        auto min = *std::min_element(image.data.begin(), image.data.end());
        auto max = *std::max_element(image.data.begin(), image.data.end());
        float factor = 0.092;

        for (int i = 0; i < width; i++) {
            for (int j = 0; j < height; j++) {
                Color color = image.get_pixel(i, j);

                color.r = (color.r - min) / (max - min);
                color.g = (color.g - min) / (max - min);
                color.b = (color.b - min) / (max - min);

                color.r = std::pow(color.r, 1.0 / 1.05);
                color.g = std::pow(color.g, 1.0 / 1.05);
                color.b = std::pow(color.b, 1.0 / 1.05);

                color.r = color.r * 255.0 * factor * std::min(width, height);
                color.g = color.g * 255.0 * factor * std::min(width, height);
                color.b = color.b * 255.0 * factor * std::min(width, height);

                image.set_pixel(i, j, color);
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
    double th1 = deg2rad(-0.1);
    double th2 = deg2rad(-0.001);
    /*double th2 = deg2rad(-0..0);*/
    double w1 = 0.0;
    double w2 = 0.0;
    double t_stop = 11;
    double FPS = 60;
    double dt = t_stop / (20.0 * FPS);

    int pendulum_nb = 1500000;
    std::vector<std::vector<PendulumState>> states_all;

    for (int i = 0; i < pendulum_nb; ++i) {
        th1 = deg2rad(9.5 + i * (10.5 - 9.5) / pendulum_nb);
        auto states = simulate_double_pendulum(G, L1, L2, M1, M2, th1, th2, w1, w2, t_stop, dt);
        states_all.push_back(states);
    }

    int const width = 1080 * 2;
    int const height = 1080 * 2;
    int const centerX = width / 2;
    int const centerY = height / 2;

    int N = 32;
    int total_frames = states_all[0].size();
    int frames_per_thread = total_frames / N;

    std::vector<std::thread> threads;
    std::mutex print_mutex;

    auto start_time = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < N; ++i) {
        int start = i * frames_per_thread;
        int end = (i == N - 1) ? total_frames : (i + 1) * frames_per_thread;
        threads.emplace_back(
            process_frames,
            start,
            end,
            std::ref(states_all),
            pendulum_nb,
            width,
            height,
            centerX,
            centerY,
            std::ref(print_mutex)
        );
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
