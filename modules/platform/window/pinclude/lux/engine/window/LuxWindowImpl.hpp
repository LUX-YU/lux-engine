#pragma once
#include "lux/engine/window/LuxWindow.hpp"
#include "lux/engine/window/LuxWindowDefination.hpp"

#include <GLFW/glfw3.h>
#ifdef __PLATFORM_WIN32__
#   define GLFW_EXPOSE_NATIVE_WIN32
#   include <windows.h>
#   include <GLFW/glfw3native.h>
#endif

#include "lux/engine/window/GLContext.hpp"
#include "lux/engine/window/VulkanContext.hpp"
#include "lux/engine/window/ContextVisitor.hpp"

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

    struct WindowCallbacks
    {
        KeyEventCallback             key_callback;
        CursorPoitionCallback        cursor_position_callback;
        ScrollCallback               scroll_callback;
        MouseButtonCallback          mouse_button_callback;
        WindowSizeChangedCallbcak    window_size_changed_callback;
    };

    class LuxWindowImpl : protected ContextVisitor
    {
        friend class LuxWindow;
    public:
        LuxWindowImpl(LuxWindow* window, InitParameter param, std::shared_ptr<GraphicContext> context)
            : _owner(window), _parameter(std::move(param))
        {
            init(std::move(context));
        }

        LuxWindowImpl(LuxWindow* window, InitParameter param)
            : _owner(window), _parameter(std::move(param))  {}

        bool init(std::shared_ptr<GraphicContext> context)
        {
            if (_init) return true;

            _context = std::move(context);
            _context->acceptVisitor(this);

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

            _init = true;
            return true;
        }

        bool isInitialized()
        {
            return _init;
        }

        ~LuxWindowImpl()
        {
            _context->cleanUp();

            glfwDestroyWindow(_glfw_window);
        }

        void addSubwindow(std::unique_ptr<Subwindow> window)
        {
            _subwindows.push_back(std::move(window));
        }

        void subscribeKeyEvent(KeyEventCallback callback)
        {
            _callbacks.key_callback = std::move(callback);
            glfwSetKeyCallback(
                _glfw_window,
                [](GLFWwindow* window, int key, int scancode, int action, int mods) {
                    auto self = static_cast<LuxWindowImpl*>(glfwGetWindowUserPointer(window));
                    self->_callbacks.key_callback(
                        *self->_owner,
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
            _callbacks.cursor_position_callback = std::move(callback);
            glfwSetCursorPosCallback(
                _glfw_window,
                [](GLFWwindow* window, double xpos, double ypos)
                {
                    auto self = static_cast<LuxWindowImpl*>(glfwGetWindowUserPointer(window));
                    self->_callbacks.cursor_position_callback(*self->_owner, xpos, ypos);
                }
            );
        }

        void subscribeScrollCallback(ScrollCallback callback)
        {
            _callbacks.scroll_callback = std::move(callback);
            glfwSetScrollCallback(
                _glfw_window,
                [](GLFWwindow* window, double xoffset, double yoffset)
                {
                    auto self = static_cast<LuxWindowImpl*>(glfwGetWindowUserPointer(window));
                    self->_callbacks.scroll_callback(*self->_owner, xoffset, yoffset);
                }
            );
        }

        void subscribeMouseButtonCallback(MouseButtonCallback callback)
        {
            _callbacks.mouse_button_callback = std::move(callback);
            glfwSetMouseButtonCallback(
                _glfw_window,
                [](GLFWwindow* window, int button, int action, int mods)
                {
                    auto self = static_cast<LuxWindowImpl*>(glfwGetWindowUserPointer(window));
                    self->_callbacks.mouse_button_callback(
                        *self->_owner,
                        glfwMouseButtonEnumConvert(button),
                        glfwKeyStateEnumConvert(action),
                        glfwModifierKeyEnumConvert(mods)
                    );
                }
            );
        }

        void subscribeWindowSizeChangeCallback(WindowSizeChangedCallbcak callback)
        {
            _callbacks.window_size_changed_callback = std::move(callback);
            glfwSetWindowSizeCallback(
                _glfw_window,
                [](GLFWwindow* window, int width, int height)
                {
                    auto self = static_cast<LuxWindowImpl*>(glfwGetWindowUserPointer(window));
                    self->_callbacks.window_size_changed_callback(
                        *self->_owner,
                        width,
                        height
                    );
                }
            );
        }

        int exec()
        {
            while (!glfwWindowShouldClose(_glfw_window))
            {
                for (auto& subwindow : _subwindows)
                {
                    subwindow->paint();
				}
				glfwPollEvents();
			}

            return 0;
        }
    protected:
        bool visitContext(GLContext* gl_context) override
        {
            if(glfwInit() != GLFW_TRUE) return false;
            glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, gl_context->majorVersion());
            glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, gl_context->minorVersion());
            return true;
        }

        bool visitContext(VulkanContext* vulkan_context)  override
        {
            if (glfwInit() != GLFW_TRUE) return false;
            glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
            return true;
        }

    private:
        LuxWindow*          _owner;
        WindowCallbacks     _callbacks;
        GLFWwindow*         _glfw_window;
        InitParameter       _parameter;
        bool                _init{ false };
        std::shared_ptr<GraphicContext> _context;
        std::vector<std::unique_ptr<Subwindow>> _subwindows;
    };
} // namespace lux::window

