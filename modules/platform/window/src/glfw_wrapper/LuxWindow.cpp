#include <lux/engine/window/LuxWindow.hpp>
#include <lux/engine/window/LuxWindowImpl.hpp>
#include <thread>

#include <atomic>
#include <stdexcept>

namespace lux::window
{
    /**
     * Start LuxWindow Defination
     */
    LuxWindow::LuxWindow(int width, int height, std::string title, std::shared_ptr<GraphicContext> context)
    {
        InitParameter parameter;
        parameter.width  = width; 
        parameter.height = height;
        parameter.title  = std::move(title);

        _impl = std::make_unique<LuxWindowImpl>(this, std::move(parameter), std::move(context));
    }

    LuxWindow::LuxWindow(const InitParameter& parameter, std::shared_ptr<GraphicContext> context)
    {
        _impl = std::make_unique<LuxWindowImpl>(this, parameter, std::move(context));
    }

    LuxWindow::LuxWindow(int width, int height, std::string title)
    {
        InitParameter parameter;
        parameter.width  = width; 
        parameter.height = height;
        parameter.title  = std::move(title);

        _impl = std::make_unique<LuxWindowImpl>(this, std::move(parameter));
    }

    LuxWindow::LuxWindow(const InitParameter& parameter)
    {
        _impl = std::make_unique<LuxWindowImpl>(this, parameter);
    }

    LuxWindow::~LuxWindow() = default;

    bool LuxWindow::init(std::shared_ptr<GraphicContext> context)
    {
        return _impl->init(std::move(context));
    }

    bool LuxWindow::is_initialized()
    {
        return _impl->is_initialized();
    }

    std::string LuxWindow::title() const
    {
        return _impl->parameter.title;
    }

    bool LuxWindow::shouldClose()
    {
        return glfwWindowShouldClose(_impl->glfw_window);
    }

    void LuxWindow::swapBuffer()
    {
        glfwSwapBuffers(_impl->glfw_window);
    }

    void LuxWindow::hideCursor(bool enable)
    {
        glfwSetInputMode(
            _impl->glfw_window, GLFW_CURSOR, 
            enable ? GLFW_CURSOR_DISABLED: GLFW_CURSOR_NORMAL
        );
    }

    void LuxWindow::enableVsync(bool enable)
    {
        glfwSwapInterval(enable?1:0);
    }

    void LuxWindow::pollEvents()
    {
        glfwPollEvents();
    }

    void LuxWindow::waitEvents()
    {
        glfwWaitEvents();
    }

    double LuxWindow::timeAfterFirstInitialization()
    {
        return glfwGetTime();
    }

    KeyState LuxWindow::queryKey(KeyEnum key) const
    {
        return glfwKeyStateEnumConvert(
            glfwGetKey(_impl->glfw_window, static_cast<int>(key))
        );
    }

    WindowSize LuxWindow::windowSize() const
    {
        WindowSize size;
        glfwGetWindowSize(_impl->glfw_window, &size.width, &size.height);
        return size;
    }

    std::string LuxWindow::windowFrameworkName() const
    {
        static const std::string framework_type = "glfw";
        return framework_type;
    }

    void LuxWindow::subscribeKeyEvent(KeyEventCallback callback)
    {
        _impl->subscribeKeyEvent(std::move(callback));
    }

    void LuxWindow::subscribeCursorPositionCallback(CursorPoitionCallback callback)
    {
        _impl->subscribeCursorPositionCallback(std::move(callback));
    }

    void LuxWindow::subscribeScrollCallback(ScrollCallback callback)
    {
        _impl->subscribeScrollCallback(std::move(callback));
    }

    void LuxWindow::subscribeMouseButtonCallback(MouseButtonCallback callback)
    {
        _impl->subscribeMouseButtonCallback(std::move(callback));
    }

    void LuxWindow::subscribeWindowSizeChangeCallback(WindowSizeChangedCallbcak callback)
    {
        _impl->subscribeWindowSizeChangeCallback(std::move(callback));
    }

    #ifdef __PLATFORM_WIN32__
    void  LuxWindow::win32Windows(void** out)
    {
        *out = static_cast<void*>(glfwGetWin32Window(_impl->glfw_window));
    }
    #endif

    GLFWwindow* LuxWindow::currentContext()
    {
        return glfwGetCurrentContext();
    }

    LuxWindow::ProcPtr LuxWindow::getProcAddress(const char* procname)
    {
        return glfwGetProcAddress(procname);
    }
}
