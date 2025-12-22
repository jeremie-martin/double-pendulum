# Claude Context for Double Pendulum

## Project Overview

C++20 double pendulum physics simulation with GPU-accelerated rendering. Simulates millions of pendulums with slightly different initial conditions to visualize chaotic divergence.

## Architecture

```
┌─────────────┐     ┌──────────────┐     ┌─────────────┐
│   Physics   │────▶│  GLRenderer  │────▶│ VideoWriter │
│  (CPU, MT)  │     │ (GPU, GLSL)  │     │  (FFmpeg)   │
└─────────────┘     └──────────────┘     └─────────────┘
      │                    │
      ▼                    ▼
┌─────────────┐     ┌──────────────┐
│  Variance   │     │    Post-     │
│  Tracker    │     │  Processing  │
└─────────────┘     │   (Shader)   │
                    └──────────────┘
```

## Key Files

| File | Purpose |
|------|---------|
| `src/simulation.cpp` | Main simulation loop, coordinates physics + rendering |
| `src/gl_renderer.cpp` | GPU line rendering with GLSL shaders |
| `src/headless_gl.cpp` | EGL context for headless GPU rendering |
| `include/pendulum.h` | RK4 physics integration (Lagrangian mechanics) |
| `include/config.h` | TOML config parsing, all parameters |
| `include/variance_tracker.h` | Chaos detection via angle variance |

## Rendering Pipeline

1. **Line Drawing**: Pendulum arms → GPU quads with smoothstep AA
2. **Accumulation**: Additive blending to RGBA32F texture
3. **Post-Processing** (GPU shader):
   - Normalize by max value
   - Apply exposure (2^stops)
   - Tone mapping (Reinhard/ACES/Logarithmic)
   - Contrast adjustment
   - Gamma correction
4. **Output**: Read back RGB8 → PNG or FFmpeg

## Common Modifications

### Add new tone mapping operator
1. Add enum value in `include/config.h` → `ToneMapOperator`
2. Add parsing in `src/config.cpp` → `parseToneMapOperator()`
3. Add GLSL implementation in `src/gl_renderer.cpp` → `pp_fragment_shader_src`
4. Add CPU reference in `include/post_process.h` → `toneMap()`

### Add new color scheme
1. Add enum in `include/config.h` → `ColorScheme`
2. Add parsing in `src/config.cpp`
3. Add implementation in `include/color_scheme.h` → `ColorSchemeGenerator`

### Modify physics
- All physics in `include/pendulum.h` → `Pendulum::step()`
- Uses RK4 integration with Lagrangian equations of motion
- Thread-safe (each pendulum is independent)

## Build Commands

```bash
# CLI only
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j

# With GUI
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_GUI=ON && cmake --build build -j
```

## Dependencies

Required: OpenGL 3.3+, GLEW, EGL, libpng, pthreads
Optional: SDL2 (GUI), FFmpeg (video output)

## Code Style

- C++20 with `auto`, structured bindings, `std::optional`
- Header-only where sensible (pendulum.h, post_process.h)
- TOML for config, JSON for metadata/progress
- No exceptions in hot paths, use return values

## Performance Notes

- Physics is CPU-bound (RK4 per pendulum per substep)
- Rendering is GPU-bound (millions of line quads)
- I/O can dominate for PNG output (use video format)
- 1M pendulums @ 4K: ~5s/frame (RTX 4090)
