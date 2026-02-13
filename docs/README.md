# Double Pendulum Chaos Simulation

GPU-accelerated physics simulation visualizing chaotic divergence. Millions of pendulums with slightly different initial conditions create striking caustic patterns.

## Quick Start

```bash
# Build (CLI only)
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j

# Run simulation
./build/pendulum config/default.toml

# With GUI (optional)
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_GUI=ON && cmake --build build -j
./build/pendulum-gui
```

## What It Does

Simulates N double pendulums (typically 100K-1M) starting from nearly identical angles. Due to chaos, tiny differences grow exponentially, causing trajectories to diverge dramatically. The visualization shows this as beautiful geometric patterns that evolve from order to chaos.

The "boom" is the peak moment when pendulums spread into a caustic pattern—this is detected automatically and can be synchronized with music.

## Binaries

| Binary | Purpose |
|--------|---------|
| `pendulum` | Main simulation with GPU rendering |
| `pendulum-gui` | Interactive preview and analysis |
| `pendulum-metrics` | Recompute metrics from saved data |
| `pendulum-optimize` | Parameter optimization via grid search |
| `pendulum-stability` | Metric stability analysis |

## Output

Each run creates `output/run_XXXX/` containing:
- `video.mp4` - Rendered simulation
- `metadata.json` - Parameters, boom frame, quality scores
- `metrics.csv` - Per-frame metric values
- `config.toml` - Effective configuration used

## Dependencies

**Required:** OpenGL 3.3+, GLEW, EGL, libpng, pthreads, zstd

**Optional:** SDL2 (GUI), FFmpeg (video output)

## Documentation

- [ARCHITECTURE.md](ARCHITECTURE.md) - How the simulation works
- [CONFIGURATION.md](CONFIGURATION.md) - Config reference and CLI tools
