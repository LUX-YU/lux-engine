#pragma once
#include "lux/window/LuxWindow.hpp"
#include "lux/window/LuxWindowDefination.hpp"

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
        KeyEventCallback             key_callback;
        CursorPoitionCallback        cursor_position_callback;
        ScrollCallback               scroll_callback;
        MouseButtonCallback          mouse_button_callback;
        WindowSizeChangedCallbcak    window_size_changed_callback;
    };

    class LuxWindowImpl
    {
    public:
        LuxWindowImpl(LuxWindow* window, InitParameter param, std::shared_ptr<GraphicContext> context)
            : parameter(std::move(param)), owner(window)
        {
            init(std::move(context));
        }

        LuxWindowImpl(LuxWindow* window, InitParameter param)
            : parameter(std::move(param)), owner(window){}

        bool init(std::shared_ptr<GraphicContext> context)
        {
            if(_init) return true;

            if(!context->init())
                return false;

            _context = std::move(context);

            auto* cw = glfwGetCurrentContext();

            glfw_window = glfwCreateWindow(
                parameter.width, parameter.height, parameter.title.c_str(), nullptr, cw
            );

            if(!glfw_window) return false;

            _context->makeCurrentContext(this);

            glfwSetWindowUserPointer(glfw_window, this);

            _init = true;
            return true;
        }

        bool is_initialized()
        {
            return _init;
        }

        ~LuxWindowImpl()
        {
            glfwDestroyWindow(glfw_window);
        }

        void subscribeKeyEvent(KeyEventCallback callback)
        {
            callbacks.key_callback = std::move(callback);
            glfwSetKeyCallback(
                glfw_window, 
                [](GLFWwindow* window, int key, int scancode, int action, int mods){
                    auto self = static_cast<LuxWindowImpl*>(glfwGetWindowUserPointer(window));
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
                    auto self = static_cast<LuxWindowImpl*>(glfwGetWindowUserPointer(window));
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
                    auto self = static_cast<LuxWindowImpl*>(glfwGetWindowUserPointer(window));
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
                    auto self = static_cast<LuxWindowImpl*>(glfwGetWindowUserPointer(window));
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
                    auto self = static_cast<LuxWindowImpl*>(glfwGetWindowUserPointer(window));
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
        bool                _init{false};
        std::shared_ptr<GraphicContext> _context;
    };
} // namespace lux::window

