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

    bool GLContext::acceptVisitor(ContextVisitor* visitor)
    {
        return visitor->visitContext(this);
    }

    bool GLContext::apiInit()
    {
        return true;
    }

    int GLContext::majorVersion()
    {
        return _major_version;
    }

    int GLContext::minorVersion()
    {
        return _minor_version;
    }

    void GLContext::cleanUp() {}
} // namespace lux::window
