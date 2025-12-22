#include "headless_gl.h"

#include <iostream>

HeadlessGL::~HeadlessGL() {
    shutdown();
}

bool HeadlessGL::init() {
    // Get default display
    display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display_ == EGL_NO_DISPLAY) {
        std::cerr << "EGL: Failed to get display\n";
        return false;
    }

    // Initialize EGL
    EGLint major, minor;
    if (!eglInitialize(display_, &major, &minor)) {
        std::cerr << "EGL: Failed to initialize\n";
        return false;
    }

    std::cout << "EGL version: " << major << "." << minor << "\n";

    // Choose config for offscreen rendering
    EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0,
        EGL_NONE
    };

    EGLConfig config;
    EGLint num_configs;
    if (!eglChooseConfig(display_, config_attribs, &config, 1, &num_configs) || num_configs == 0) {
        std::cerr << "EGL: Failed to choose config\n";
        eglTerminate(display_);
        return false;
    }

    // Bind OpenGL API
    if (!eglBindAPI(EGL_OPENGL_API)) {
        std::cerr << "EGL: Failed to bind OpenGL API\n";
        eglTerminate(display_);
        return false;
    }

    // Try OpenGL 4.3 first (for compute shaders), fallback to 3.3
    EGLint context_attribs_43[] = {
        EGL_CONTEXT_MAJOR_VERSION, 4,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE
    };

    context_ = eglCreateContext(display_, config, EGL_NO_CONTEXT, context_attribs_43);
    if (context_ == EGL_NO_CONTEXT) {
        // Fallback to OpenGL 3.3
        EGLint context_attribs_33[] = {
            EGL_CONTEXT_MAJOR_VERSION, 3,
            EGL_CONTEXT_MINOR_VERSION, 3,
            EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
            EGL_NONE
        };
        context_ = eglCreateContext(display_, config, EGL_NO_CONTEXT, context_attribs_33);
        if (context_ == EGL_NO_CONTEXT) {
            std::cerr << "EGL: Failed to create context\n";
            eglTerminate(display_);
            return false;
        }
    }

    // Create a small pbuffer surface (we render to FBO anyway)
    EGLint pbuffer_attribs[] = {
        EGL_WIDTH, 1,
        EGL_HEIGHT, 1,
        EGL_NONE
    };

    surface_ = eglCreatePbufferSurface(display_, config, pbuffer_attribs);
    if (surface_ == EGL_NO_SURFACE) {
        std::cerr << "EGL: Failed to create pbuffer surface\n";
        eglDestroyContext(display_, context_);
        eglTerminate(display_);
        return false;
    }

    // Make context current
    if (!makeCurrent()) {
        std::cerr << "EGL: Failed to make context current\n";
        shutdown();
        return false;
    }

    // Initialize GLEW
    glewExperimental = GL_TRUE;
    GLenum err = glewInit();
    if (err != GLEW_OK) {
        std::cerr << "GLEW: Failed to initialize: " << glewGetErrorString(err) << "\n";
        shutdown();
        return false;
    }

    std::cout << "OpenGL version: " << glGetString(GL_VERSION) << "\n";
    std::cout << "OpenGL renderer: " << glGetString(GL_RENDERER) << "\n";

    initialized_ = true;
    return true;
}

void HeadlessGL::shutdown() {
    if (display_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

        if (surface_ != EGL_NO_SURFACE) {
            eglDestroySurface(display_, surface_);
            surface_ = EGL_NO_SURFACE;
        }

        if (context_ != EGL_NO_CONTEXT) {
            eglDestroyContext(display_, context_);
            context_ = EGL_NO_CONTEXT;
        }

        eglTerminate(display_);
        display_ = EGL_NO_DISPLAY;
    }

    initialized_ = false;
}

bool HeadlessGL::makeCurrent() {
    return eglMakeCurrent(display_, surface_, surface_, context_) == EGL_TRUE;
}
