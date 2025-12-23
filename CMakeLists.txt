cmake_minimum_required(VERSION 3.16)
project(double-pendulum VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler flags
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DDEBUG")

# Find required packages
find_package(Threads REQUIRED)
find_package(OpenGL REQUIRED)
find_package(GLEW REQUIRED)
find_package(PNG REQUIRED)
# EGL is required for headless GPU rendering
find_library(EGL_LIBRARY EGL REQUIRED)
# ZSTD for simulation data compression
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZSTD REQUIRED libzstd)

# Metrics library sources (unified metrics system)
set(METRICS_SOURCES
    src/metrics/metric_series.cpp
    src/metrics/metrics_collector.cpp
    src/metrics/event_detector.cpp
    src/metrics/boom_analyzer.cpp
    src/metrics/causticness_analyzer.cpp
    src/metrics/metrics_view.cpp
    src/metrics/probe_filter.cpp
    src/metrics/probe_pipeline.cpp
)

# Core library sources (shared between all executables)
set(CORE_SOURCES
    src/config.cpp
    src/simulation.cpp
    src/simulation_data.cpp
    src/video_writer.cpp
    src/music_manager.cpp
    src/batch_generator.cpp
    src/preset_library.cpp
    src/headless_gl.cpp
    src/gl_renderer.cpp
    ${METRICS_SOURCES}
)

# Core static library
add_library(double-pendulum-core STATIC ${CORE_SOURCES})

target_include_directories(double-pendulum-core PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/external
    ${ZSTD_INCLUDE_DIRS}
)

target_link_libraries(double-pendulum-core PUBLIC
    Threads::Threads
    OpenGL::GL
    GLEW::GLEW
    PNG::PNG
    ${EGL_LIBRARY}
    ${ZSTD_LIBRARIES}
)

target_compile_options(double-pendulum-core PRIVATE
    -Wall -Wextra -Wpedantic
)

# Main executable (CLI with GPU rendering)
add_executable(pendulum src/main.cpp)

target_link_libraries(pendulum PRIVATE double-pendulum-core)

target_compile_options(pendulum PRIVATE
    -Wall -Wextra -Wpedantic
)

# Metrics iteration executable
add_executable(pendulum-metrics src/main_metrics.cpp)

target_link_libraries(pendulum-metrics PRIVATE double-pendulum-core)

target_compile_options(pendulum-metrics PRIVATE
    -Wall -Wextra -Wpedantic
)

# GUI executable (optional)
option(BUILD_GUI "Build GUI application with ImGui" OFF)
if(BUILD_GUI)
    find_package(SDL2 REQUIRED)

    # ImGui sources (GUI-specific)
    set(IMGUI_SOURCES
        external/imgui/imgui.cpp
        external/imgui/imgui_draw.cpp
        external/imgui/imgui_tables.cpp
        external/imgui/imgui_widgets.cpp
        external/imgui/backends/imgui_impl_sdl2.cpp
        external/imgui/backends/imgui_impl_opengl3.cpp
        external/implot/implot.cpp
        external/implot/implot_items.cpp
    )

    add_executable(pendulum-gui
        src/main_gui.cpp
        ${IMGUI_SOURCES}
    )

    target_include_directories(pendulum-gui PRIVATE
        ${SDL2_INCLUDE_DIRS}
        ${CMAKE_SOURCE_DIR}/external/imgui
        ${CMAKE_SOURCE_DIR}/external/imgui/backends
        ${CMAKE_SOURCE_DIR}/external/implot
    )

    target_link_libraries(pendulum-gui PRIVATE
        double-pendulum-core
        ${SDL2_LIBRARIES}
    )

    target_compile_options(pendulum-gui PRIVATE
        -Wall -Wextra -Wpedantic
    )
endif()

# Install
install(TARGETS pendulum DESTINATION bin)
install(DIRECTORY config/ DESTINATION share/double-pendulum/config)
