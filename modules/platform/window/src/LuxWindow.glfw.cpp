#include <lux/engine/window/LuxWindow.hpp>
#include <lux/engine/window/LuxWindowDefination.hpp>

#include <thread>

// Include Windows headers before GLFW to avoid APIENTRY macro redefinition warning.
// minwindef.h (pulled in by windows.h) and glfw3.h both define APIENTRY; whichever
// comes second triggers C4005.  Windows SDK headers must win.
#ifdef _WIN32
#   ifndef WIN32_LEAN_AND_MEAN
#       define WIN32_LEAN_AND_MEAN
#   endif
#   include <windows.h>
#endif

// vulkan.h must precede glfw3.h so GLFW exposes glfwCreateWindowSurface
// (guarded by VK_VERSION_1_0). GLFW itself loads Vulkan dynamically — this
// backend needs the Vulkan headers only, not the loader library.
#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>
// #include "lux/engine/window/VulkanContext.hpp"
#include <cassert>
#ifdef __PLATFORM_WIN32__
#   define GLFW_EXPOSE_NATIVE_WIN32
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

    static ModifierKey glfwModifierKeyMaskConvert(int mods)
    {
        int result = 0;
        if (mods & GLFW_MOD_SHIFT)     result |= ModifierKey::KEY_MOD_SHIFT;
        if (mods & GLFW_MOD_CONTROL)   result |= ModifierKey::KEY_MOD_CONTROL;
        if (mods & GLFW_MOD_ALT)       result |= ModifierKey::KEY_MOD_ALT;
        if (mods & GLFW_MOD_SUPER)     result |= ModifierKey::KEY_MOD_SUPER;
        if (mods & GLFW_MOD_CAPS_LOCK) result |= ModifierKey::KEY_MOD_CAPS_LOCK;
        if (mods & GLFW_MOD_NUM_LOCK)  result |= ModifierKey::KEY_MOD_NUM_LOCK;
        return static_cast<ModifierKey>(result);
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

    void LuxWindow::window_close_callback(GLFWwindow* window) 
    {
        // hide the window
        LuxWindow* window_impl = (LuxWindow*)glfwGetWindowUserPointer(window);

        if (window_impl->_exit_behavior == EExitBehavior::EXIT)
        {
            return;
        }
        else if(window_impl->_exit_behavior == EExitBehavior::HIDE)
        {
            glfwSetWindowShouldClose(window, false);
            glfwHideWindow(window);
        }
    }

    /**
     * Start LuxWindow Defination
     */
    LuxWindow::LuxWindow(int width, int height, std::string title)
        : _parameter{ width, height, std::move(title) }
    {
        _init = init();
    }

    LuxWindow::LuxWindow(common::Size2D size, std::string title)
        : LuxWindow(size.width, size.height, std::move(title))
    {

    }

    LuxWindow::LuxWindow(const InitParameter& parameter)
        : _parameter{ parameter }
    {
        _init = init();
    }

    LuxWindow::~LuxWindow()
    {
        if (_glfw_window)
        {
            glfwDestroyWindow(_glfw_window);
            _glfw_window = nullptr;
        }
    }

    bool LuxWindow::init()
    {
        if (_init) return true;

        // GLFW must already be initialized via GlfwRuntime before creating a window.

        // Checked BEFORE creating the window: a machine without a Vulkan loader
        // can never drive this window, and flashing one on screen only to
        // return false leaves the user staring at a frame that vanishes.
        if (!glfwVulkanSupported())
        {
            init_error_ = EWindowInitError::VULKAN_UNAVAILABLE;
            return false;
        }

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

        auto* cw = glfwGetCurrentContext();

        _glfw_window = glfwCreateWindow(
            _parameter.width, _parameter.height,
            _parameter.title.c_str(), nullptr, cw
        );

        if (!_glfw_window)
        {
            init_error_ = EWindowInitError::BACKEND_CREATE_FAILED;
            return false;
        }

        glfwSetWindowUserPointer(_glfw_window, this);
        glfwSetWindowCloseCallback(_glfw_window, &LuxWindow::window_close_callback);

        // Enable CapsLock / NumLock modifier bits in key/mouse callbacks.
        glfwSetInputMode(_glfw_window, GLFW_LOCK_KEY_MODS, GLFW_TRUE);

        subscribeKeyEvent();
        subscribeCursorPositionCallback();
        subscribeScrollCallback();
        subscribeMouseButtonCallback();
        subscribeCharCallback();
        subscribeWindowSizeChangeCallback();
        subscribeFramebufferSizeChangeCallback();
        subscribeDropCallback();

        init_error_ = EWindowInitError::NONE;
        return true;
    }

    bool LuxWindow::vulkanSupported()
    {
        return glfwVulkanSupported();
    }

    bool LuxWindow::createVulkanSurface(VkInstance instance,
                                        const VkAllocationCallbacks* allocator,
                                        VkSurfaceKHR* out_surface)
    {
        *out_surface = VK_NULL_HANDLE;
        if (!_init || _glfw_window == nullptr)
            return false;
        const VkResult result = glfwCreateWindowSurface(instance, _glfw_window, allocator, out_surface);
        if (result != VK_SUCCESS || *out_surface == VK_NULL_HANDLE)
        {
            *out_surface = VK_NULL_HANDLE;
            return false;
        }
        return true;
    }

    std::span<const char* const> LuxWindow::requiredVulkanInstanceExtensions()
    {
        if (glfwInit() != GLFW_TRUE)  // idempotent when already initialized
            return {};
        uint32_t count = 0;
        const char** extensions = glfwGetRequiredInstanceExtensions(&count);
        if (extensions == nullptr)
            return {};
        return {extensions, count};
    }

    bool LuxWindow::isInitialized() const
    {
        return _init;
    }

    const char* LuxWindow::title() const
    {
        return _parameter.title.c_str();
    }

    bool LuxWindow::shouldClose()
    {
        return glfwWindowShouldClose(_glfw_window);
    }

    void LuxWindow::hideCursor(bool enable)
    {
        glfwSetInputMode(
            _glfw_window, GLFW_CURSOR,
            enable ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL
        );
    }

    bool LuxWindow::setRawMouseMotion(bool enable)
    {
        if (!glfwRawMouseMotionSupported()) {
            return false;
        }
        glfwSetInputMode(_glfw_window, GLFW_RAW_MOUSE_MOTION, enable ? GLFW_TRUE : GLFW_FALSE);
        return true;
    }

    void LuxWindow::getCursorPos(double* x, double* y) const
    {
        glfwGetCursorPos(_glfw_window, x, y);
    }

    void LuxWindow::setCursorPos(double x, double y)
    {
        glfwSetCursorPos(_glfw_window, x, y);
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

    common::Size2D LuxWindow::size() const
    {
		int width, height;
        glfwGetWindowSize(_glfw_window, &width, &height);
        common::Size2D size{
            .width  = (uint32_t)width,
            .height = (uint32_t)height
        };
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

                // Maintain live held state and accumulate edge events.
                // GLFW may pass key == -1 for unknown keys; guard against it.
                if (key >= 0 && key < static_cast<int>(kKeyboardBitCount))
                {
                    const auto idx = static_cast<size_t>(key);
                    if (action == GLFW_PRESS)
                    {
                        self->_live_keys_held.set(idx);
                        self->_pending_keys_pressed.set(idx);
                    }
                    else if (action == GLFW_RELEASE)
                    {
                        self->_live_keys_held.reset(idx);
                        self->_pending_keys_released.set(idx);
                    }
                    else if (action == GLFW_REPEAT)
                    {
                        // REPEAT does not create a new press edge;
                        // it only appears in the events vector.
                        self->_live_keys_held.set(idx);
                    }
                }

                KeyAction data{
                    glfwKeyEnumConvert(key),
                    scancode,
                    glfwKeyStateEnumConvert(action),
                    glfwModifierKeyMaskConvert(mods)
                };
                self->_pending_events.emplace_back(data);
                if (self->on_key) self->on_key(KeyEvent{data});
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
                if (self->on_cursor_move) self->on_cursor_move(CursorMoveEvent{xpos, ypos});
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
                MouseScrollAction data{xoffset, yoffset};
                self->_pending_events.emplace_back(data);
                self->_pending_scroll_dx += xoffset;
                self->_pending_scroll_dy += yoffset;
                if (self->on_mouse_scroll) self->on_mouse_scroll(MouseScrollEvent{data});
            }
        );
    }

    void LuxWindow::subscribeDropCallback()
    {
        glfwSetDropCallback(
            _glfw_window,
            [](GLFWwindow* window, int count, const char** paths)
            {
                auto self = static_cast<LuxWindow*>(glfwGetWindowUserPointer(window));
                FileDropEvent ev;
                ev.paths.reserve(static_cast<size_t>(count));
                for (int i = 0; i < count; ++i)
                    ev.paths.emplace_back(paths[i]); // GLFW gives absolute paths
                if (self->on_file_drop) self->on_file_drop(ev);
            }
        );
    }

    void LuxWindow::subscribeCharCallback()
    {
        glfwSetCharCallback(
            _glfw_window,
            [](GLFWwindow* window, unsigned int codepoint)
            {
                auto self = static_cast<LuxWindow*>(glfwGetWindowUserPointer(window));
                self->_pending_char_inputs.push_back(CharInput{.codepoint=codepoint});
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

                if (button >= 0 && button < kMouseButtonCount)
                {
                    const uint8_t bit = static_cast<uint8_t>(1u << button);
                    if (action == GLFW_PRESS)
                    {
                        self->_live_mouse_held |= bit;
                        self->_pending_mouse_pressed |= bit;
                    }
                    else if (action == GLFW_RELEASE)
                    {
                        self->_live_mouse_held = static_cast<uint8_t>(
                            self->_live_mouse_held & ~bit);
                        self->_pending_mouse_released |= bit;
                    }
                }

                MouseButtonAction data{
                    glfwMouseButtonEnumConvert(button),
                    glfwKeyStateEnumConvert(action),
                    glfwModifierKeyMaskConvert(mods)
                };
                self->_pending_events.emplace_back(data);
                if (self->on_mouse_button) self->on_mouse_button(MouseButtonEvent{data});
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
                if (self->on_resize) self->on_resize(WindowResizeEvent{common::Size2D{(uint32_t)width, (uint32_t)height}});
            }
        );
    }

    void LuxWindow::subscribeFramebufferSizeChangeCallback()
    {
        glfwSetFramebufferSizeCallback(
            _glfw_window,
            [](GLFWwindow* window, int width, int height)
            {
                auto self = static_cast<LuxWindow*>(glfwGetWindowUserPointer(window));
                if (self->on_framebuffer_resize) self->on_framebuffer_resize(FramebufferResizeEvent{common::Size2D{(uint32_t)width, (uint32_t)height}});
            }
        );
    }

    float LuxWindow::lastFrameDelayTime() const
    {
        return _delta_time;
    }

    common::Size2D LuxWindow::framebufferSize() const
    {
		int width, height;
        glfwGetFramebufferSize(_glfw_window, &width, &height);
        common::Size2D size{
            .width  = (uint32_t)width,
            .height = (uint32_t)height
        };
        return size;
    }

#ifdef __PLATFORM_WIN32__
    void*  LuxWindow::win32Handle()
    {
        return (void*)glfwGetWin32Window(_glfw_window);
    }
#endif

    GLFWwindow* LuxWindow::handle()
    {
        return _glfw_window;
    }

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

            glfwPollEvents();

            if (on_draw_ready) on_draw_ready(DrawReadyEvent{});

            newFrame();

            if (on_draw_finished) on_draw_finished(DrawFinishedEvent{});
        }

        return 0;
    }

    void LuxWindow::exit()
    {
        _exit_behavior = EExitBehavior::EXIT;
        glfwSetWindowShouldClose(_glfw_window, GLFW_TRUE);
    }

    void LuxWindow::hide(bool var)
    {
        var ? glfwHideWindow(_glfw_window) : glfwShowWindow(_glfw_window);
    }

    void LuxWindow::setExitBehavior(EExitBehavior behavior)
    {
        _exit_behavior = behavior;
    }

    void LuxWindow::newFrame(){}

    InputSnapshot LuxWindow::captureInputSnapshot()
    {
        InputSnapshot snap;

        // --- Keyboard: held + edge (callback-driven, no polling) ------------
        snap.keys_held          = _live_keys_held;
        snap.keys_just_pressed  = _pending_keys_pressed;
        snap.keys_just_released = _pending_keys_released;
        _pending_keys_pressed.reset();
        _pending_keys_released.reset();

        // --- Mouse buttons: held + edge (callback-driven) -------------------
        snap.mouse_held          = _live_mouse_held;
        snap.mouse_just_pressed  = _pending_mouse_pressed;
        snap.mouse_just_released = _pending_mouse_released;
        _pending_mouse_pressed  = 0;
        _pending_mouse_released = 0;

        // --- Cursor position & delta ----------------------------------------
        double cx = 0.0, cy = 0.0;
        glfwGetCursorPos(_glfw_window, &cx, &cy);
        snap.cursor_x  = cx;
        snap.cursor_y  = cy;
        snap.cursor_dx = _snapshot_initialized ? (cx - _snapshot_cursor_x) : 0.0;
        snap.cursor_dy = _snapshot_initialized ? (cy - _snapshot_cursor_y) : 0.0;
        _snapshot_cursor_x = cx;
        _snapshot_cursor_y = cy;

        // --- Scroll delta (accumulated by callback since last capture) -------
        snap.scroll_dx     = _pending_scroll_dx;
        snap.scroll_dy     = _pending_scroll_dy;
        _pending_scroll_dx = 0.0;
        _pending_scroll_dy = 0.0;

        // --- Window / framebuffer size --------------------------------------
        {
            const auto ws = size();
            snap.window_width  = ws.width;
            snap.window_height = ws.height;
            const auto fs = framebufferSize();
            snap.framebuffer_width  = fs.width;
            snap.framebuffer_height = fs.height;
        }

        // --- Discrete events (drained from the callback buffer) -------------
        snap.events = std::move(_pending_events);
        _pending_events.clear();  // leave capacity for next frame

        // --- Character input (drained from glfwSetCharCallback buffer) ------
        snap.text_events = std::move(_pending_char_inputs);
        _pending_char_inputs.clear();

        // --- Timing ---------------------------------------------------------
        const double now = timeAfterFirstInitialization();
        snap.sample_timestamp = now;
        snap.sample_dt = _snapshot_initialized
                   ? static_cast<float>(now - _snapshot_time)
                   : 0.0f;
        _snapshot_time        = now;
        _snapshot_initialized = true;

        return snap;
    }
}
