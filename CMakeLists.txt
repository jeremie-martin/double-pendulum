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

# Find packages
find_package(Threads REQUIRED)

# GPU rendering option (uses EGL for headless OpenGL)
option(ENABLE_GPU "Enable GPU-accelerated rendering via EGL" OFF)

# Main executable
set(PENDULUM_SOURCES
    src/main.cpp
    src/config.cpp
    src/simulation.cpp
    src/video_writer.cpp
    src/music_manager.cpp
    src/batch_generator.cpp
)

if(ENABLE_GPU)
    find_package(OpenGL REQUIRED)
    find_package(GLEW REQUIRED)
    find_package(PNG REQUIRED)
    # EGL is typically part of the OpenGL/Mesa package
    find_library(EGL_LIBRARY EGL REQUIRED)

    list(APPEND PENDULUM_SOURCES
        src/headless_gl.cpp
        src/gl_renderer.cpp
    )
endif()

add_executable(pendulum ${PENDULUM_SOURCES})

target_include_directories(pendulum PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/external
)

target_link_libraries(pendulum PRIVATE
    Threads::Threads
)

if(ENABLE_GPU)
    target_link_libraries(pendulum PRIVATE
        OpenGL::GL
        GLEW::GLEW
        PNG::PNG
        ${EGL_LIBRARY}
    )
    target_compile_definitions(pendulum PRIVATE ENABLE_GPU)
endif()

# Enable warnings
target_compile_options(pendulum PRIVATE
    -Wall -Wextra -Wpedantic
)

# GUI executable (Phase 3 - optional)
option(BUILD_GUI "Build GUI application with ImGui" OFF)
if(BUILD_GUI)
    find_package(SDL2 REQUIRED)
    find_package(OpenGL REQUIRED)
    find_package(GLEW REQUIRED)

    add_executable(pendulum-gui
        src/main_gui.cpp
        src/config.cpp
        src/simulation.cpp
        src/video_writer.cpp
        src/music_manager.cpp
        src/gl_renderer.cpp
        external/imgui/imgui.cpp
        external/imgui/imgui_draw.cpp
        external/imgui/imgui_tables.cpp
        external/imgui/imgui_widgets.cpp
        external/imgui/backends/imgui_impl_sdl2.cpp
        external/imgui/backends/imgui_impl_opengl3.cpp
    )

    target_include_directories(pendulum-gui PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/external
        ${SDL2_INCLUDE_DIRS}
        ${CMAKE_SOURCE_DIR}/external/imgui
        ${CMAKE_SOURCE_DIR}/external/imgui/backends
    )

    target_link_libraries(pendulum-gui PRIVATE
        Threads::Threads
        ${SDL2_LIBRARIES}
        OpenGL::GL
        GLEW::GLEW
    )

    target_compile_options(pendulum-gui PRIVATE
        -Wall -Wextra -Wpedantic
    )
endif()

# Install
install(TARGETS pendulum DESTINATION bin)
install(DIRECTORY config/ DESTINATION share/double-pendulum/config)
