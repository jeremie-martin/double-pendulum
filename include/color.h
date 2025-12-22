#pragma once

// Simple RGB color struct used throughout the codebase
struct Color {
    float r, g, b;

    Color operator*(float s) const { return {r * s, g * s, b * s}; }
    Color operator+(Color const& o) const { return {r + o.r, g + o.g, b + o.b}; }
};
