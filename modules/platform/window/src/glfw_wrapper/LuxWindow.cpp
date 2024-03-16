#include <lux/engine/window/LuxWindow.hpp>
#include <lux/engine/window/LuxWindowDefination.hpp>

// graphics api context
#include <lux/engine/window/GLContext.hpp>
#include <lux/engine/window/VulkanContext.hpp>
#include <lux/engine/window/ContextVisitor.hpp>

#include <thread>

#include <GLFW/glfw3.h>
#ifdef __PLATFORM_WIN32__
#   define GLFW_EXPOSE_NATIVE_WIN32
#   include <windows.h>
#   include <GLFW/glfw3native.h>
#endif

namespace lux::window
{
    static KeyEnum glfwKeyEnumConvert(int key)
    {
        return static_cast<KeyEnum>(key);
    }

    static KeyState glfwKeyStateEnumConvert(int key)
    {
        switch (key)
        {
        case GLFW_RELEASE:
            return KeyState::RELEASE;
        case GLFW_PRESS:
            return KeyState::PRESS;
        case GLFW_REPEAT:
            return KeyState::REPEAT;
        }
        return KeyState::UNKNOWN;
    }

    static ModifierKey glfwModifierKeyEnumConvert(int key)
    {
        switch (key)
        {
        case GLFW_MOD_SHIFT:
            return ModifierKey::KEY_MOD_SHIFT;
        case GLFW_MOD_CONTROL:
            return ModifierKey::KEY_MOD_CONTROL;
        case GLFW_MOD_ALT:
            return ModifierKey::KEY_MOD_ALT;
        case GLFW_MOD_SUPER:
            return ModifierKey::KEY_MOD_SUPER;
        case GLFW_MOD_CAPS_LOCK:
            return ModifierKey::KEY_MODE_CAPS_LOCK;
        case GLFW_MOD_NUM_LOCK:
            return ModifierKey::KEY_MOD_NUM_LOCK;
        }
        return ModifierKey::UNKNOWN;
    }

    static MouseButton glfwMouseButtonEnumConvert(int key)
    {
        switch (key)
        {
        case GLFW_MOUSE_BUTTON_1:
            return MouseButton::MOUSE_BUTTON_LEFT;
        case GLFW_MOUSE_BUTTON_2:
            return MouseButton::MOUSE_BUTTON_RIGHT;
        case GLFW_MOUSE_BUTTON_3:
            return MouseButton::MOUSE_BUTTON_MIDDLE;
        case GLFW_MOUSE_BUTTON_4:
            return MouseButton::MOUSE_BUTTON_4;
        case GLFW_MOUSE_BUTTON_5:
            return MouseButton::MOUSE_BUTTON_5;
        case GLFW_MOUSE_BUTTON_6:
            return MouseButton::MOUSE_BUTTON_6;
        case GLFW_MOUSE_BUTTON_7:
            return MouseButton::MOUSE_BUTTON_7;
        case GLFW_MOUSE_BUTTON_8:
            return MouseButton::MOUSE_BUTTON_8;
        default:
            break;
        }
        return MouseButton::UNKNOWN;
    }

    enum class ContextOperation
    {
        GLFW_INIT
    };

    /**
     * Start LuxWindow Defination
     */
    LuxWindow::LuxWindow(int width, int height, std::string title, std::shared_ptr<GraphicContext> context)
    {
        _parameter.width  = width;
        _parameter.height = height;
        _parameter.title  = std::move(title);

        _init = init(std::move(context));
    }

    LuxWindow::LuxWindow(const InitParameter& parameter, std::shared_ptr<GraphicContext> context)
    {
        _parameter = parameter;

        _init = init(std::move(context));
    }

    LuxWindow::LuxWindow(int width, int height, std::string title)
    {
        _parameter.width = width;
        _parameter.height = height;
        _parameter.title = std::move(title);
    }

    LuxWindow::LuxWindow(const InitParameter& parameter)
    {
        _parameter = parameter;
    }

    LuxWindow::~LuxWindow()
    {
        _context->cleanUp();

        glfwDestroyWindow(_glfw_window);
    }

    bool LuxWindow::init(std::shared_ptr<GraphicContext> context)
    {
        if (_init) return true;

        _context = std::move(context);
        _context->acceptVisitor(this, static_cast<int>(ContextOperation::GLFW_INIT));

        auto* cw = glfwGetCurrentContext();

        _glfw_window = glfwCreateWindow(
            _parameter.width, _parameter.height,
            _parameter.title.c_str(), nullptr, cw
        );
        if (!_glfw_window) return false;

        if (!_context->apiInit())
        {
            _context->cleanUp();

            glfwDestroyWindow(_glfw_window);

            return false;
        }

        glfwMakeContextCurrent(_glfw_window);
        glfwSetWindowUserPointer(_glfw_window, this);

        subscribeKeyEvent();
        subscribeCursorPositionCallback();
        subscribeScrollCallback();
        subscribeMouseButtonCallback();
        subscribeWindowSizeChangeCallback();

        return true;
    }

    bool LuxWindow::isInitialized() const
    {
        return _init;
    }

    std::string LuxWindow::title() const
    {
        return _parameter.title;
    }

    bool LuxWindow::shouldClose()
    {
        return glfwWindowShouldClose(_glfw_window);
    }

    void LuxWindow::swapBuffer()
    {
        glfwSwapBuffers(_glfw_window);
    }

    void LuxWindow::hideCursor(bool enable)
    {
        glfwSetInputMode(
            _glfw_window, GLFW_CURSOR,
            enable ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL
        );
    }

    void LuxWindow::enableVsync(bool enable)
    {
        glfwSwapInterval(enable ? 1 : 0);
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
            glfwGetKey(_glfw_window, static_cast<int>(key))
        );
    }

    WindowSize LuxWindow::size() const
    {
        WindowSize size;
        glfwGetWindowSize(_glfw_window, &size.width, &size.height);
        return size;
    }

    std::string LuxWindow::windowFrameworkName() const
    {
        static const std::string framework_type = "glfw";
        return framework_type;
    }

    void LuxWindow::subscribeKeyEvent()
    {
        glfwSetKeyCallback(
            _glfw_window,
            [](GLFWwindow* window, int key, int scancode, int action, int mods) {
                auto self = static_cast<LuxWindow*>(glfwGetWindowUserPointer(window));
                auto event = std::make_unique<KeyEvent>(
                    KeyAction{
					    glfwKeyEnumConvert(key),
					    scancode,
					    glfwKeyStateEnumConvert(action),
					    glfwModifierKeyEnumConvert(mods)
					}
				);
                self->dispatch(std::move(event));
            }
        );
    }

    void LuxWindow::subscribeCursorPositionCallback()
    {
        glfwSetCursorPosCallback(
            _glfw_window,
            [](GLFWwindow* window, double xpos, double ypos)
            {
                auto self = static_cast<LuxWindow*>(glfwGetWindowUserPointer(window));
                self->dispatch(std::make_unique<CursorPositionChangedEvent>(CursorPositionChanged{ xpos, ypos }));
            }
        );
    }

    void LuxWindow::subscribeScrollCallback()
    {
        glfwSetScrollCallback(
            _glfw_window,
            [](GLFWwindow* window, double xoffset, double yoffset)
            {
                auto self = static_cast<LuxWindow*>(glfwGetWindowUserPointer(window));
                self->dispatch(std::make_unique<MouseScrollEvent>(MouseScrollAction{xoffset, yoffset}));
            }
        );
    }

    void LuxWindow::subscribeMouseButtonCallback()
    {
        glfwSetMouseButtonCallback(
            _glfw_window,
            [](GLFWwindow* window, int button, int action, int mods)
            {
                auto self = static_cast<LuxWindow*>(glfwGetWindowUserPointer(window));
                auto event = std::make_unique<MouseButtonEvent>(
                    MouseButtonAction {
                        glfwMouseButtonEnumConvert(button),
                        glfwKeyStateEnumConvert(action),
                        glfwModifierKeyEnumConvert(mods)
                    }
                );
                
                self->dispatch(std::move(event));
            }
        );
    }

    void LuxWindow::subscribeWindowSizeChangeCallback()
    {
        glfwSetWindowSizeCallback(
            _glfw_window,
            [](GLFWwindow* window, int width, int height)
            {
                auto self = static_cast<LuxWindow*>(glfwGetWindowUserPointer(window));
                self->dispatch(std::make_unique<WindowSizeChangedEvent>(WindowSize{ width, height }));
            }
        );
    }

    float LuxWindow::lastFrameDelayTime() const
    {
        return _delta_time;
    }

    WindowSize LuxWindow::framebufferSize() const
    {
        WindowSize size;
        glfwGetFramebufferSize(_glfw_window, &size.width, &size.height);
        return size;
    }

#ifdef __PLATFORM_WIN32__
    void  LuxWindow::win32Windows(void** out)
    {
        *out = _glfw_window;
    }
#endif

    GLFWwindow* LuxWindow::currentContext()
    {
        return glfwGetCurrentContext();
    }

    void LuxWindow::makeContextCurrent(GLFWwindow* context)
    {
        glfwMakeContextCurrent(context);
    }

    LuxWindow::ProcPtr LuxWindow::getProcAddress(const char* procname)
    {
        return glfwGetProcAddress(procname);
    }

    int LuxWindow::exec()
    {
        while (!glfwWindowShouldClose(_glfw_window))
        {
            float current_time = timeAfterFirstInitialization();
            _delta_time        = current_time - _last_frame_time;
            _last_frame_time   = current_time;

            dispatch(std::make_unique<DrawReadyEvent>());

            glfwPollEvents();

            newFrame();

            glfwSwapBuffers(_glfw_window);

            dispatch(std::make_unique<DrawFinishedEvent>());
        }

        return 0;
    }

    bool LuxWindow::visitContext(GLContext* gl_context, int operation)
    {
        if (operation == static_cast<int>(ContextOperation::GLFW_INIT))
        {
            if (glfwInit() != GLFW_TRUE) return false;
            glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, gl_context->majorVersion());
            glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, gl_context->minorVersion());
            return true;
        }

        return false;
    }

    bool LuxWindow::visitContext(VulkanContext* vulkan_context, int operation)
    {
        if (operation == static_cast<int>(ContextOperation::GLFW_INIT))
        {
            if (glfwInit() != GLFW_TRUE) return false;
            glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
            return true;
        }

        return false;
    }

    void LuxWindow::newFrame(){}
}
