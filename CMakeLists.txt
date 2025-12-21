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

# Main executable (mostly header-only, just main.cpp and config.cpp)
add_executable(pendulum
    src/main.cpp
    src/config.cpp
)

target_include_directories(pendulum PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/external
)

target_link_libraries(pendulum PRIVATE
    Threads::Threads
)

# Enable warnings
target_compile_options(pendulum PRIVATE
    -Wall -Wextra -Wpedantic
)

# GUI executable (Phase 2 - optional)
option(BUILD_GUI "Build GUI application with ImGui" OFF)
if(BUILD_GUI)
    find_package(SDL2 REQUIRED)
    find_package(OpenGL REQUIRED)

    add_executable(pendulum-gui
        src/main_gui.cpp
        src/config.cpp
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
    )
endif()

# Install
install(TARGETS pendulum DESTINATION bin)
install(DIRECTORY config/ DESTINATION share/double-pendulum/config)
