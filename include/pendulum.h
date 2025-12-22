#pragma once

#include <cmath>
#include <vector>

struct PendulumState {
    double x1, y1, x2, y2; // Cartesian positions
    double th1, th2;       // Angles (for boom detection)
};

class Pendulum {
public:
    Pendulum()
        : G(9.81), L1(1.0), L2(1.0), M1(1.0), M2(1.0), th1(0.0), th2(0.0), w1(0.0), w2(0.0) {}

    Pendulum(double G, double L1, double L2, double M1, double M2, double th1, double th2,
             double w1, double w2)
        : G(G), L1(L1), L2(L2), M1(M1), M2(M2), th1(th1), th2(th2), w1(w1), w2(w2) {}

    // RK4 integration step - more accurate than Euler
    PendulumState step(double dt) {
        // k1
        auto [a1_k1, a2_k1] = accelerations(th1, th2, w1, w2);

        // k2
        double th1_k2 = th1 + w1 * dt / 2;
        double th2_k2 = th2 + w2 * dt / 2;
        double w1_k2 = w1 + a1_k1 * dt / 2;
        double w2_k2 = w2 + a2_k1 * dt / 2;
        auto [a1_k2, a2_k2] = accelerations(th1_k2, th2_k2, w1_k2, w2_k2);

        // k3
        double th1_k3 = th1 + w1_k2 * dt / 2;
        double th2_k3 = th2 + w2_k2 * dt / 2;
        double w1_k3 = w1 + a1_k2 * dt / 2;
        double w2_k3 = w2 + a2_k2 * dt / 2;
        auto [a1_k3, a2_k3] = accelerations(th1_k3, th2_k3, w1_k3, w2_k3);

        // k4
        double th1_k4 = th1 + w1_k3 * dt;
        double th2_k4 = th2 + w2_k3 * dt;
        double w1_k4 = w1 + a1_k3 * dt;
        double w2_k4 = w2 + a2_k3 * dt;
        auto [a1_k4, a2_k4] = accelerations(th1_k4, th2_k4, w1_k4, w2_k4);

        // Update state using weighted average
        th1 += dt / 6.0 * (w1 + 2 * w1_k2 + 2 * w1_k3 + w1_k4);
        th2 += dt / 6.0 * (w2 + 2 * w2_k2 + 2 * w2_k3 + w2_k4);
        w1 += dt / 6.0 * (a1_k1 + 2 * a1_k2 + 2 * a1_k3 + a1_k4);
        w2 += dt / 6.0 * (a2_k1 + 2 * a2_k2 + 2 * a2_k3 + a2_k4);

        return computeState();
    }

    // Generate N steps
    std::vector<PendulumState> steps(int N, double dt) {
        std::vector<PendulumState> states;
        states.reserve(N);
        for (int i = 0; i < N; ++i) {
            states.push_back(step(dt));
        }
        return states;
    }

    // Get current state without stepping
    PendulumState currentState() const { return computeState(); }

    // Accessors
    double getTheta1() const { return th1; }
    double getTheta2() const { return th2; }
    double getOmega1() const { return w1; }
    double getOmega2() const { return w2; }

private:
    double G, L1, L2, M1, M2;
    double th1, th2, w1, w2;

    // Compute angular accelerations using Lagrangian mechanics
    std::pair<double, double> accelerations(double theta1, double theta2, double omega1,
                                            double omega2) const {
        // Angular acceleration of first pendulum
        double num1 = -G * (2 * M1 + M2) * std::sin(theta1);
        double num2 = -M2 * G * std::sin(theta1 - 2 * theta2);
        double num3 = -2 * std::sin(theta1 - theta2) * M2;
        double num4 = omega2 * omega2 * L2 + omega1 * omega1 * L1 * std::cos(theta1 - theta2);
        double den = L1 * (2 * M1 + M2 - M2 * std::cos(2 * theta1 - 2 * theta2));
        double a1 = (num1 + num2 + num3 * num4) / den;

        // Angular acceleration of second pendulum
        num1 = 2 * std::sin(theta1 - theta2);
        num2 = omega1 * omega1 * L1 * (M1 + M2);
        num3 = G * (M1 + M2) * std::cos(theta1);
        num4 = omega2 * omega2 * L2 * M2 * std::cos(theta1 - theta2);
        den = L2 * (2 * M1 + M2 - M2 * std::cos(2 * theta1 - 2 * theta2));
        double a2 = (num1 * (num2 + num3 + num4)) / den;

        return {a1, a2};
    }

    PendulumState computeState() const {
        double x1 = L1 * std::sin(th1);
        double y1 = L1 * std::cos(th1);
        double x2 = x1 + L2 * std::sin(th2);
        double y2 = y1 + L2 * std::cos(th2);
        return {x1, y1, x2, y2, th1, th2};
    }
};
