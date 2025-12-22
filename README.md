# Double Pendulum Simulation

A high-performance C++ simulation of chaotic double pendulum dynamics with GPU-accelerated rendering and beautiful spectral coloring.

## Features

- **GPU Rendering**: Hardware-accelerated line rendering with anti-aliased quads
- **RK4 Integration**: Accurate 4th-order Runge-Kutta physics simulation
- **Parallel Processing**: Multi-threaded physics, GPU-accelerated rendering
- **HDR Pipeline**: Float32 accumulation with tone mapping (Reinhard, ACES, Logarithmic)
- **Multiple Color Schemes**: Spectrum, Rainbow, Heat, Cool, Monochrome
- **Batch Generation**: Automated generation of multiple videos with randomized parameters
- **Music Sync**: Automatic music muxing with beat-drop alignment to chaos onset
- **GUI Preview**: Real-time interactive preview with ImGui
- **Headless Mode**: EGL-based rendering for servers without display

## Building

### CLI (required)

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### GUI (optional)

```bash
cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_GUI=ON
make -j$(nproc)
```

## Dependencies

- C++20 compiler (GCC 10+, Clang 12+)
- CMake 3.16+
- OpenGL 3.3+
- GLEW
- libpng
- EGL (for headless rendering)
- FFmpeg (for video output)
- SDL2, ImGui (for GUI, included in external/)

## Usage

```bash
# Run with default config
./pendulum

# Run with custom config
./pendulum config/my_config.toml

# Run with music (syncs to boom frame)
./pendulum config/default.toml --music random
./pendulum config/default.toml --music petrunko

# Batch generation
./pendulum --batch config/batch.toml
./pendulum --batch config/batch.toml --resume

# Add music to existing video
./pendulum --add-music output/video.mp4 petrunko 32 60

# List available music tracks
./pendulum --list-tracks

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
total_frames = 660
substeps_per_frame = 200

[render]
width = 2160
height = 2160

[post_process]
tone_map = "none"  # none, reinhard, reinhard_extended, aces, logarithmic
exposure = 0.0     # stops (+/- brightness)
contrast = 1.0
gamma = 2.2

[color]
scheme = "spectrum"  # spectrum, rainbow, heat, cool, monochrome
start = 0.0
end = 1.0

[output]
format = "video"  # video or png
directory = "output"
video_fps = 60
```

## Project Structure

```
├── src/           # Source files
├── include/       # Header files
├── config/        # TOML configuration files
├── external/      # Third-party (toml++, nlohmann/json, imgui, stb)
├── music/         # Music tracks for muxing
└── output/        # Generated videos/frames
```

## License

GPL-3.0
