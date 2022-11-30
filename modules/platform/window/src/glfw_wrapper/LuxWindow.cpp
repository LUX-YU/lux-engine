#include <lux-engine/platform/window/LuxWindow.hpp>
#include <thread>

#include <GLFW/glfw3.h>
#ifdef __PLATFORM_WIN32__
#   define GLFW_EXPOSE_NATIVE_WIN32
#   include <windows.h>
#   include <GLFW/glfw3native.h>
#endif

#include <atomic>
#include <stdexcept>

namespace lux::engine::platform
{
    static KeyEnum glfwKeyEnumConvert(int key)
    {
        return static_cast<KeyEnum>(key);
    }

    static KeyState glfwKeyStateEnumConvert(int key)
    {
        switch(key)
        {
        case GLFW_RELEASE :
            return KeyState::RELEASE;
        case GLFW_PRESS :
            return KeyState::PRESS;
        case GLFW_REPEAT :
            return KeyState::REPEAT;
        }
        return KeyState::UNKNOWN;
    }

    static ModifierKey glfwModifierKeyEnumConvert(int key)
    {
        switch(key)
        {
        case GLFW_MOD_SHIFT :
            return ModifierKey::KEY_MOD_SHIFT ;
        case GLFW_MOD_CONTROL :
            return ModifierKey::KEY_MOD_CONTROL;
        case GLFW_MOD_ALT :
            return ModifierKey::KEY_MOD_ALT;
        case GLFW_MOD_SUPER :
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

    struct WindowCallbacks
    {
        LuxWindow::KeyEventCallback             key_callback;
        LuxWindow::CursorPoitionCallback        cursor_position_callback;
        LuxWindow::ScrollCallback               scroll_callback;
        LuxWindow::MouseButtonCallback          mouse_button_callback;
        LuxWindow::WindowSizeChangedCallbcak    window_size_changed_callback;
    };

    class LuxWindow::Impl
    {
    public:
        static void glfw_opengl_init()
        {
            static bool is_init{false};
            if(!is_init)
            {
                if(glfwInit() != GLFW_TRUE)
                {
                    throw std::runtime_error("glfw init error");
                }
                glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
                glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
                is_init = true;
            }
        }

        static void glfw_no_api()
        {
            static bool is_init{false};
            if(!is_init)
            {
                if(glfwInit() != GLFW_TRUE)
                {
                    throw std::runtime_error("glfw init error");
                }
                glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
                is_init = true;
            }
        }

        inline void initialize_method(lux::engine::platform::GraphicAPI api)
        {
            switch (api)
            {
            case GraphicAPI::OPENGL :
                glfw_opengl_init();
                break;
            case GraphicAPI::DIRECTX :
                break;
            case GraphicAPI::VULKAN :
                glfw_no_api();
                break;
            default:
                break;
            }
        }

        Impl(LuxWindow* window, InitParameter param)
            : parameter(std::move(param)), owner(window)
        {
            // this method only call once in one process
            initialize_method(parameter.graphic_api);
            glfw_window = glfwCreateWindow(parameter.width, parameter.height, parameter.title.c_str(), nullptr, nullptr);

            glfwMakeContextCurrent(glfw_window);
            glfwSetWindowUserPointer(glfw_window, this);
            #ifdef __PLATFORM_WIN32__
            windows_handle = glfwGetWin32Window(glfw_window);
            #endif
        }

        ~Impl()
        {
            glfwDestroyWindow(glfw_window);
        }

        void subscribeKeyEvent(KeyEventCallback callback)
        {
            callbacks.key_callback = std::move(callback);
            glfwSetKeyCallback(
                glfw_window, 
                [](GLFWwindow* window, int key, int scancode, int action, int mods){
                    auto self = static_cast<Impl*>(glfwGetWindowUserPointer(window));
                    self->callbacks.key_callback(
                        *self->owner, 
                        glfwKeyEnumConvert(key), 
                        scancode, 
                        glfwKeyStateEnumConvert(action),
                        glfwModifierKeyEnumConvert(mods)
                    );
                }
            );
        }

        void subscribeCursorPositionCallback(CursorPoitionCallback callback)
        {
            callbacks.cursor_position_callback = std::move(callback);
            glfwSetCursorPosCallback(
                glfw_window,
                [](GLFWwindow* window, double xpos, double ypos)
                {
                    auto self = static_cast<Impl*>(glfwGetWindowUserPointer(window));
                    self->callbacks.cursor_position_callback(*self->owner, xpos, ypos);
                }
            );
        }

        void subscribeScrollCallback(ScrollCallback callback)
        {
            callbacks.scroll_callback = std::move(callback);
            glfwSetScrollCallback(
                glfw_window,
                [](GLFWwindow* window, double xoffset, double yoffset)
                {
                    auto self = static_cast<Impl*>(glfwGetWindowUserPointer(window));
                    self->callbacks.scroll_callback(*self->owner, xoffset, yoffset);
                }
            );
        }

        void subscribeMouseButtonCallback(MouseButtonCallback callback)
        {
            callbacks.mouse_button_callback = std::move(callback);
            glfwSetMouseButtonCallback(
                glfw_window,
                [](GLFWwindow* window, int button, int action, int mods)
                {
                    auto self = static_cast<Impl*>(glfwGetWindowUserPointer(window));
                    self->callbacks.mouse_button_callback(
                        *self->owner, 
                        glfwMouseButtonEnumConvert(button),
                        glfwKeyStateEnumConvert(action),
                        glfwModifierKeyEnumConvert(mods)
                    );
                }
            );
        }

        void subscribeWindowSizeChangeCallback(WindowSizeChangedCallbcak callback)
        {
            callbacks.window_size_changed_callback = std::move(callback);
            glfwSetWindowSizeCallback(
                glfw_window,
                [](GLFWwindow* window, int width, int height)
                {
                    auto self = static_cast<Impl*>(glfwGetWindowUserPointer(window));
                    self->callbacks.window_size_changed_callback(
                        *self->owner,
                        width,
                        height
                    );
                }
            );
        }

        LuxWindow*          owner;
        WindowCallbacks     callbacks;
        GLFWwindow*         glfw_window;
        InitParameter       parameter;

        #ifdef __PLATFORM_WIN32__
        HWND                windows_handle;
        #endif
    };

    /**
     * Start LuxWindow Defination
     */

    LuxWindow::LuxWindow(int width, int height, std::string title, GraphicAPI api_type)
    {
        InitParameter parameter;
        parameter.width = width; parameter.height = height;
        parameter.title = std::move(title);
        parameter.graphic_api = api_type;
        _impl = std::make_unique<Impl>(this, std::move(parameter));
    }

    LuxWindow::LuxWindow(const InitParameter& parameter)
    {
        _impl = std::make_unique<Impl>(this, parameter);
    }

    LuxWindow::~LuxWindow() = default;

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

    GraphicAPI LuxWindow::graphicAPIType() const
    {
        return _impl->parameter.graphic_api;
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
    void  LuxWindow::win32Windows(void* out)
    {
        out = static_cast<void*>(_impl->windows_handle);
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
