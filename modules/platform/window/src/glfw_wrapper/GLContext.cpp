#include "lux/engine/window/GLContext.hpp"
#include "lux/engine/window/LuxWindowImpl.hpp"
#include <GLFW/glfw3.h>

namespace lux::window
{
    GLContext::GLContext(int major, int minor)
    {
        _major_version = major;
        _minor_version = minor;
    }

    bool GLContext::init()
    {
        if(!_init)
        {
            if(glfwInit() != GLFW_TRUE) return false;
            glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, _major_version);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, _minor_version);
            _init = true;
        }

        return true;
    }

    void GLContext::makeCurrentContext(LuxWindowImpl* window) 
    {
        glfwMakeContextCurrent(window->glfw_window);
    }
} // namespace lux::window
