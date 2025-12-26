# Double Pendulum Simulation

A high-performance C++ simulation of chaotic double pendulum dynamics with GPU-accelerated rendering.

## Features

- **GPU Rendering**: Hardware-accelerated line rendering with anti-aliased quads
- **RK4 Physics**: 4th-order Runge-Kutta integration with Lagrangian mechanics
- **Parallel Processing**: Multi-threaded physics, GPU-accelerated rendering
- **HDR Pipeline**: Float32 accumulation with tone mapping (Reinhard, ACES, Logarithmic)
- **Batch Generation**: Automated video generation with quality filtering
- **GUI Preview**: Real-time interactive preview with ImGui
- **Headless Mode**: EGL-based rendering for servers without display

## Building

```bash
# CLI only
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j

# With GUI
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_GUI=ON && cmake --build build -j
```

## Dependencies

- C++20 compiler (GCC 10+, Clang 12+)
- CMake 3.16+
- OpenGL 3.3+, GLEW, EGL, libpng, zstd
- FFmpeg (video output)
- SDL2 (GUI only)

## Usage

```bash
# Run simulation
./pendulum config/default.toml

# Batch generation
./pendulum --batch config/batch.toml

# GUI preview
./pendulum-gui
```

## Configuration

See `config/default.toml` for all options:

```toml
[physics]
gravity = 9.81
length1 = 1.0
length2 = 1.0
initial_angle1_deg = 200.2
initial_angle2_deg = 200.0

[simulation]
pendulum_count = 100000
angle_variation_deg = 0.1
duration_seconds = 11.0
physics_quality = "high"

[render]
width = 2160
height = 2160

[color]
scheme = "spectrum"
start = 0.0
end = 1.0

[output]
format = "video"
video_fps = 60
```

## Documentation

See `CLAUDE.md` for detailed architecture, metrics system, and developer documentation.

## License

GPL-3.0
