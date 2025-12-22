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

    // Compute total mechanical energy (kinetic + potential)
    // Energy should be conserved in ideal simulation (useful for validation)
    double totalEnergy() const {
        // Reference: ground level is at y = 0 (pivot point)
        // Potential energy is negative below pivot

        // Positions (y increases downward in our convention)
        double y1 = -L1 * std::cos(th1);  // Height of first bob (negative = below pivot)
        double y2 = y1 - L2 * std::cos(th2);  // Height of second bob

        // Potential energy (PE = mgh, with h measured upward from pivot)
        double PE = -M1 * G * y1 - M2 * G * y2;

        // Velocities of first bob
        double v1x = L1 * w1 * std::cos(th1);
        double v1y = L1 * w1 * std::sin(th1);
        double v1_sq = v1x * v1x + v1y * v1y;

        // Velocities of second bob (relative to first + first's velocity)
        double v2x = v1x + L2 * w2 * std::cos(th2);
        double v2y = v1y + L2 * w2 * std::sin(th2);
        double v2_sq = v2x * v2x + v2y * v2y;

        // Kinetic energy (KE = 0.5 * m * v^2)
        double KE = 0.5 * M1 * v1_sq + 0.5 * M2 * v2_sq;

        return KE + PE;
    }

private:
    double G, L1, L2, M1, M2;
    double th1, th2, w1, w2;

    // Compute angular accelerations using Lagrangian mechanics
    std::pair<double, double> accelerations(double theta1, double theta2, double omega1,
                                            double omega2) const {
        // Precompute common angle differences
        double delta = theta1 - theta2;
        double delta2 = 2.0 * delta;  // = 2*theta1 - 2*theta2

        // Use sincos for efficiency (computes both in ~same time as one)
        double sin_theta1, cos_theta1;
        double sin_delta, cos_delta;
        double sin_delta2, cos_delta2;
        double sin_t1_minus_2t2;

        sincos(theta1, &sin_theta1, &cos_theta1);
        sincos(delta, &sin_delta, &cos_delta);
        sincos(delta2, &sin_delta2, &cos_delta2);
        sin_t1_minus_2t2 = std::sin(theta1 - 2.0 * theta2);

        // Common denominator factor
        double denom_factor = 2.0 * M1 + M2 - M2 * cos_delta2;

        // Angular acceleration of first pendulum
        double num1 = -G * (2.0 * M1 + M2) * sin_theta1;
        double num2 = -M2 * G * sin_t1_minus_2t2;
        double num3 = -2.0 * sin_delta * M2;
        double num4 = omega2 * omega2 * L2 + omega1 * omega1 * L1 * cos_delta;
        double a1 = (num1 + num2 + num3 * num4) / (L1 * denom_factor);

        // Angular acceleration of second pendulum
        double n1 = 2.0 * sin_delta;
        double n2 = omega1 * omega1 * L1 * (M1 + M2);
        double n3 = G * (M1 + M2) * cos_theta1;
        double n4 = omega2 * omega2 * L2 * M2 * cos_delta;
        double a2 = (n1 * (n2 + n3 + n4)) / (L2 * denom_factor);

        return {a1, a2};
    }

    PendulumState computeState() const {
        // Use sincos for efficiency
        double sin_th1, cos_th1, sin_th2, cos_th2;
        sincos(th1, &sin_th1, &cos_th1);
        sincos(th2, &sin_th2, &cos_th2);

        double x1 = L1 * sin_th1;
        double y1 = L1 * cos_th1;
        double x2 = x1 + L2 * sin_th2;
        double y2 = y1 + L2 * cos_th2;
        return {x1, y1, x2, y2, th1, th2};
    }
};
