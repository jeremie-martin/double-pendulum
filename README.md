# Double Pendulum Simulation

A high-performance C++ simulation of chaotic double pendulum dynamics, visualized with beautiful spectral coloring.

## Features

- **RK4 Integration**: Accurate 4th-order Runge-Kutta physics simulation
- **Parallel Processing**: Multi-threaded simulation and rendering
- **Configurable**: TOML configuration files for all parameters
- **Multiple Color Schemes**: Spectrum, Rainbow, Heat, Cool, Monochrome
- **Post-Processing**: Gamma correction, brightness normalization, contrast
- **Boom Detection**: Automatic detection of chaos onset (divergence)
- **Video Output**: Direct MP4 generation via FFmpeg
- **Streaming Mode**: Memory-efficient frame-by-frame processing

## Building

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

## Usage

```bash
# Run with default config
./build/pendulum

# Run with custom config
./build/pendulum config/my_config.toml

# Quick test
./build/pendulum config/test.toml
```

## Configuration

See `config/default.toml` for all available options:

```toml
[physics]
gravity = 9.81
length1 = 1.0
length2 = 1.0
initial_angle1_deg = -32.2
initial_angle2_deg = -32.0

[simulation]
pendulum_count = 100000
angle_variation_deg = 0.1
duration_seconds = 11.0
fps = 60

[render]
width = 2160
height = 2160

[color]
scheme = "spectrum"  # spectrum, rainbow, heat, cool, monochrome

[output]
format = "png"  # or "video"
directory = "output"
```

## Dependencies

- C++20 compiler (GCC 10+, Clang 12+)
- CMake 3.16+
- pthreads
- FFmpeg (optional, for video output)

## License

GPL-3.0
