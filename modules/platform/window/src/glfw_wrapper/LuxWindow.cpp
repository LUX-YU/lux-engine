#include <lux-engine/platform/window/LuxWindow.hpp>
#include <thread>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

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
        case GLFW_PRESS :
            return KeyState::PRESS;
        case GLFW_RELEASE :
            return KeyState::RELEASE;
        case GLFW_REPEAT :
            return KeyState::REPEAT;
        }
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
    }


    struct WindowCallbacks
    {
        LuxWindow::KeyEventCallback        key_callback;
        LuxWindow::CursorPoitionCallback   cursor_position_callback;
        LuxWindow::ScrollCallback          scroll_callback;
    };

    class LuxWindow::Impl
    {
    public:
        Impl(int width, int height, std::string title, GraphicAPI api)
        : width(width), height(height), _title(std::move(title))
        {
            glfw_window = glfwCreateWindow(width, height, _title.c_str(), nullptr, nullptr);
            if(api == GraphicAPI::OPENGL)
            {
                glfwMakeContextCurrent(glfw_window);
            }
        }

        ~Impl()
        {
            glfwDestroyWindow(glfw_window);
        }

        WindowCallbacks     callbacks;
        GLFWwindow*         glfw_window;
        GraphicAPI          api_type;
        std::string         _title;
        int                 width;
        int                 height;
    };

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

    LuxWindow::LuxWindow(int width, int height, std::string title, GraphicAPI api_type)
    {
        switch (api_type)
        {
        case GraphicAPI::OPENGL :
            glfw_opengl_init();
            break;
        case GraphicAPI::DIRECTX :
        case GraphicAPI::VULKAN :
            glfw_no_api();
            break;
        default:
            break;
        }
        _common_constructor(width, height, std::move(title), api_type);
    }

    void LuxWindow::_common_constructor(int width, int height, std::string title, GraphicAPI api)
    {
        _impl = std::make_unique<Impl>(width, height, title, api);
        glfwSetWindowUserPointer(_impl->glfw_window, this);
    }

    LuxWindow::~LuxWindow() = default;

    std::string LuxWindow::title()
    {
        return _impl->_title;
    }

    bool LuxWindow::shouldClose()
    {
        return glfwWindowShouldClose(_impl->glfw_window);
    }

    void LuxWindow::swapBuffer()
    {
        glfwSwapBuffers(_impl->glfw_window);
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

    float LuxWindow::timeAfterFirstInitialization()
    {
        return glfwGetTime();
    }

    KeyState LuxWindow::queryKey(KeyEnum key)
    {
        return glfwKeyStateEnumConvert(
            glfwGetKey(_impl->glfw_window, static_cast<int>(key))
        );
    }

    void LuxWindow::subscribeKeyEvent(KeyEventCallback callback)
    {
        _impl->callbacks.key_callback = std::move(callback);

        glfwSetKeyCallback(
            _impl->glfw_window, 
            [](GLFWwindow* window, int key, int scancode, int action, int mods){
                auto self = static_cast<LuxWindow*>(glfwGetWindowUserPointer(window));
                self->_impl->callbacks.key_callback(
                    *self, 
                    glfwKeyEnumConvert(key), 
                    scancode, 
                    glfwKeyStateEnumConvert(action),
                    glfwModifierKeyEnumConvert(mods)
                );
            }
        );
    }

    void LuxWindow::subscribeCursorPositionCallback(CursorPoitionCallback callback)
    {
        _impl->callbacks.cursor_position_callback = std::move(callback);

        glfwSetCursorPosCallback(
            _impl->glfw_window,
            [](GLFWwindow* window, double xpos, double ypos)
            {
                auto self = static_cast<LuxWindow*>(glfwGetWindowUserPointer(window));
                self->_impl->callbacks.cursor_position_callback(*self, xpos, ypos);
            }
        );
    }

    void LuxWindow::subscribeScrollCallback(ScrollCallback callback)
    {
        _impl->callbacks.scroll_callback = std::move(callback);;

        glfwSetScrollCallback(
            _impl->glfw_window,
            [](GLFWwindow* window, double xoffset, double yoffset)
            {
                auto self = static_cast<LuxWindow*>(glfwGetWindowUserPointer(window));
                self->_impl->callbacks.scroll_callback(*self, xoffset, yoffset);
            }
        );
    }

    void* LuxWindow::lowLayerPointer()
    {
        return _impl->glfw_window;
    }
}
