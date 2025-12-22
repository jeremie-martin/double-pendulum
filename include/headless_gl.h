#pragma once

#include <GL/glew.h>

// EGL for headless OpenGL context
#include <EGL/egl.h>

// Headless OpenGL context using EGL (no window required)
class HeadlessGL {
public:
    HeadlessGL() = default;
    ~HeadlessGL();

    // Initialize EGL context for offscreen rendering
    bool init();

    // Clean up
    void shutdown();

    // Make this context current (call before any GL operations)
    bool makeCurrent();

    bool isInitialized() const { return initialized_; }

private:
    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLContext context_ = EGL_NO_CONTEXT;
    EGLSurface surface_ = EGL_NO_SURFACE;
    bool initialized_ = false;
};
