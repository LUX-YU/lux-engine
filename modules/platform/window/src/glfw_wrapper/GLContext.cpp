#include "lux/engine/window/GLContext.hpp"
#include "lux/engine/window/LuxWindowImpl.hpp"

namespace lux::window
{
    GLContext::GLContext(int major, int minor)
    {
        _major_version = major;
        _minor_version = minor;
    }

    GLContext::~GLContext() = default;

    bool GLContext::acceptVisitor(ContextVisitor* visitor, int operation)
    {
        return visitor->visitContext(this, operation);
    }

    GraphicAPI GLContext::apiType() const
    {
        return GraphicAPI::OpenGL3;
    }

    bool GLContext::apiInit()
    {
        _init = true;
        return true;
    }

    int GLContext::majorVersion() const
    {
        return _major_version;
    }

    int GLContext::minorVersion() const
    {
        return _minor_version;
    }

    void GLContext::cleanUp() {}
} // namespace lux::window
