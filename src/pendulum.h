#pragma once

#include <cmath>
#include <vector>

struct PendulumState
{
    double x1, y1, x2, y2;
};

class Pendulum
{
public:
    Pendulum()
        : G(0.0)
        , L1(0.0)
        , L2(0.0)
        , M1(0.0)
        , M2(0.0)
        , th1(0.0)
        , th2(0.0)
        , w1(0.0)
        , w2(0.0)
        , t(0.0) { }

    Pendulum(double G, double L1, double L2, double M1, double M2, double th1, double th2, double w1, double w2)
        : G(G)
        , L1(L1)
        , L2(L2)
        , M1(M1)
        , M2(M2)
        , th1(th1)
        , th2(th2)
        , w1(w1)
        , w2(w2)
        , t(0.0) { }

    PendulumState step(double dt) {
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

        t += dt;
        return { x1, y1, x2, y2 };
    }

    std::vector<PendulumState> steps(int N, double dt) {
        std::vector<PendulumState> states;
        for (int i = 0; i < N; ++i) {
            states.push_back(step(dt));
        }
        return states;
    }

private:
    double G, L1, L2, M1, M2;
    double th1, th2, w1, w2, t;
};
