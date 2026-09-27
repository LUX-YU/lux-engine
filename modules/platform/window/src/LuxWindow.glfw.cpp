#include <lux/engine/window/LuxWindow.hpp>

#include <thread>
#include <utility>
#include <cstdlib>

// Include Windows headers before GLFW to avoid APIENTRY macro redefinition warning.
// minwindef.h (pulled in by windows.h) and glfw3.h both define APIENTRY; whichever
// comes second triggers C4005.  Windows SDK headers must win.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <imm.h>
#endif

// vulkan.h must precede glfw3.h so GLFW exposes glfwCreateWindowSurface
// (guarded by VK_VERSION_1_0). GLFW itself loads Vulkan dynamically — this
// backend needs the Vulkan headers only, not the loader library.
#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>
// #include "lux/engine/window/VulkanContext.hpp"
#include <cassert>
#ifdef __PLATFORM_WIN32__
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

namespace lux::window
{
    void LuxWindow::window_close_callback(GLFWwindow* window)
    {
        // hide the window
        LuxWindow* window_impl = (LuxWindow*)glfwGetWindowUserPointer(window);

        if (window_impl->on_close)
        {
            // A callback owns the decision. The OS flag must not force an exit
            // while the application is still asking whether to save changes.
            glfwSetWindowShouldClose(window, GLFW_FALSE);
            window_impl->on_close(WindowCloseEvent{});
            return;
        }

        if (window_impl->_exit_behavior == EExitBehavior::EXIT)
        {
            return;
        }
        else if (window_impl->_exit_behavior == EExitBehavior::HIDE)
        {
            glfwSetWindowShouldClose(window, false);
            glfwHideWindow(window);
        }
    }

    bool LuxWindow::focused() const noexcept
    {
        return _glfw_window && glfwGetWindowAttrib(_glfw_window, GLFW_FOCUSED);
    }

    bool LuxWindow::visible() const noexcept
    {
        return _glfw_window && glfwGetWindowAttrib(_glfw_window, GLFW_VISIBLE);
    }

    bool LuxWindow::minimized() const noexcept
    {
        return _glfw_window && glfwGetWindowAttrib(_glfw_window, GLFW_ICONIFIED);
    }

    void LuxWindow::waitEvents(double timeout_seconds)
    {
        if (timeout_seconds > 0.0)
            glfwWaitEventsTimeout(timeout_seconds);
        else
            glfwPollEvents();
    }

    void LuxWindow::wakeEvents() noexcept
    {
        glfwPostEmptyEvent();
    }

    /**
     * Start LuxWindow Defination
     */
    LuxWindow::LuxWindow(int width, int height, std::string title) : _parameter{width, height, std::move(title)}
    {
        _init = init();
    }

    LuxWindow::LuxWindow(const InitParameter& parameter) : _parameter{parameter}
    {
        _init = init();
    }

    LuxWindow::~LuxWindow()
    {
        if (_glfw_window)
        {
            // On Windows, WM_NCDESTROY removes the subclass while this owner is alive.
            glfwDestroyWindow(_glfw_window);
            _glfw_window = nullptr;
        }
    }

    bool LuxWindow::init()
    {
        if (_init)
        {
            return true;
        }

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

        _glfw_window = glfwCreateWindow(_parameter.width, _parameter.height, _parameter.title.c_str(), nullptr, cw);

        if (!_glfw_window)
        {
            init_error_ = EWindowInitError::BACKEND_CREATE_FAILED;
            return false;
        }

        glfwSetWindowUserPointer(_glfw_window, this);
        glfwSetWindowCloseCallback(_glfw_window, &LuxWindow::window_close_callback);
        glfwSetWindowFocusCallback(_glfw_window, [](GLFWwindow* window, int focused) {
            auto* self = static_cast<LuxWindow*>(glfwGetWindowUserPointer(window));
            if (focused)
            {
                self->recordInput(WindowFocusEvent{});
                if (self->on_focus)
                    self->on_focus({});
            }
            else
            {
                if (std::exchange(self->composing_, false))
                    self->recordInput(WindowCompositionEvent{ECompositionStage::CANCELLED});
                self->recordInput(WindowLostFocusEvent{});
                if (self->on_lost_focus)
                    self->on_lost_focus({});
            }
        });

#if defined(_WIN32)
        const auto capture_ime =
            [](HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR context) -> LRESULT {
            auto& owner = *reinterpret_cast<LuxWindow*>(context);
            switch (message)
            {
            case WM_IME_STARTCOMPOSITION:
                owner.composing_ = true;
                owner.recordInput(WindowCompositionEvent{ECompositionStage::STARTED});
                break;
            case WM_IME_COMPOSITION:
                if (lparam & GCS_RESULTSTR)
                {
                    owner.composing_ = false;
                    owner.recordInput(WindowCompositionEvent{ECompositionStage::COMMITTED});
                }
                if (lparam & (GCS_COMPSTR | GCS_COMPATTR | GCS_COMPCLAUSE | GCS_CURSORPOS))
                {
                    owner.composing_ = true;
                    owner.recordInput(WindowCompositionEvent{ECompositionStage::UPDATED});
                }
                if (!lparam && std::exchange(owner.composing_, false))
                    owner.recordInput(WindowCompositionEvent{ECompositionStage::CANCELLED});
                break;
            case WM_KILLFOCUS:
            case WM_IME_ENDCOMPOSITION:
                if (std::exchange(owner.composing_, false))
                    owner.recordInput(WindowCompositionEvent{ECompositionStage::CANCELLED});
                break;
            case WM_NCDESTROY:
                RemoveWindowSubclass(window, reinterpret_cast<SUBCLASSPROC>(id), id);
                break;
            }
            // Do not read/submit GCS_RESULTSTR. GLFW's character callback remains
            // the sole committed-text path; Windows owns composition/candidates.
            return DefSubclassProc(window, message, wparam, lparam);
        };
        const auto procedure = static_cast<SUBCLASSPROC>(capture_ime);
        if (!SetWindowSubclass(
                glfwGetWin32Window(_glfw_window),
                procedure,
                reinterpret_cast<UINT_PTR>(procedure),
                reinterpret_cast<DWORD_PTR>(this)
            ))
        {
            glfwDestroyWindow(_glfw_window);
            _glfw_window = nullptr;
            init_error_ = EWindowInitError::BACKEND_CREATE_FAILED;
            return false;
        }
#endif

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

    bool LuxWindow::createVulkanSurface(
        VkInstance instance,
        const VkAllocationCallbacks* allocator,
        VkSurfaceKHR* out_surface
    )
    {
        *out_surface = VK_NULL_HANDLE;
        if (!_init || _glfw_window == nullptr)
        {
            return false;
        }
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
        if (glfwInit() != GLFW_TRUE) // idempotent when already initialized
        {
            return {};
        }
        uint32_t count = 0;
        const char** extensions = glfwGetRequiredInstanceExtensions(&count);
        if (extensions == nullptr)
        {
            return {};
        }
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
        glfwSetInputMode(_glfw_window, GLFW_CURSOR, enable ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    }

    bool LuxWindow::setRawMouseMotion(bool enable)
    {
        if (!glfwRawMouseMotionSupported())
        {
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

    void LuxWindow::size(std::uint32_t& width, std::uint32_t& height) const
    {
        int native_width = 0;
        int native_height = 0;
        glfwGetWindowSize(_glfw_window, &native_width, &native_height);
        width = static_cast<std::uint32_t>(native_width);
        height = static_cast<std::uint32_t>(native_height);
    }

    std::string LuxWindow::windowFrameworkName() const
    {
        static const std::string framework_type = "glfw";
        return framework_type;
    }

    void LuxWindow::subscribeKeyEvent()
    {
        glfwSetKeyCallback(_glfw_window, [](GLFWwindow* window, int key, int scancode, int action, int mods) {
            auto self = static_cast<LuxWindow*>(glfwGetWindowUserPointer(window));
            WindowKeyEvent event{key, scancode, action, mods};
            self->recordInput(event);
            if (self->on_key)
            {
                self->on_key(event);
            }
        });
    }

    void LuxWindow::subscribeCursorPositionCallback()
    {
        glfwSetCursorPosCallback(_glfw_window, [](GLFWwindow* window, double xpos, double ypos) {
            auto self = static_cast<LuxWindow*>(glfwGetWindowUserPointer(window));
            self->recordInput(CursorMoveEvent{xpos, ypos});
            if (self->on_cursor_move)
            {
                self->on_cursor_move(CursorMoveEvent{xpos, ypos});
            }
        });
    }

    void LuxWindow::subscribeScrollCallback()
    {
        glfwSetScrollCallback(_glfw_window, [](GLFWwindow* window, double xoffset, double yoffset) {
            auto self = static_cast<LuxWindow*>(glfwGetWindowUserPointer(window));
            const WindowScrollEvent event{xoffset, yoffset};
            self->recordInput(event);
            if (self->on_mouse_scroll)
            {
                self->on_mouse_scroll(event);
            }
        });
    }

    void LuxWindow::subscribeDropCallback()
    {
        glfwSetDropCallback(_glfw_window, [](GLFWwindow* window, int count, const char** paths) {
            auto self = static_cast<LuxWindow*>(glfwGetWindowUserPointer(window));
            FileDropEvent ev;
            ev.paths.reserve(static_cast<size_t>(count));
            for (int i = 0; i < count; ++i)
            {
                ev.paths.emplace_back(paths[i]); // GLFW gives absolute paths
            }
            if (self->on_file_drop)
            {
                self->on_file_drop(ev);
            }
        });
    }

    void LuxWindow::subscribeCharCallback()
    {
        glfwSetCharCallback(_glfw_window, [](GLFWwindow* window, unsigned int codepoint) {
            auto self = static_cast<LuxWindow*>(glfwGetWindowUserPointer(window));
            self->recordInput(WindowTextEvent{.codepoint = codepoint});
        });
    }

    void LuxWindow::subscribeMouseButtonCallback()
    {
        glfwSetMouseButtonCallback(_glfw_window, [](GLFWwindow* window, int button, int action, int mods) {
            auto self = static_cast<LuxWindow*>(glfwGetWindowUserPointer(window));

            WindowMouseButtonEvent event{button, action, mods};
            self->recordInput(event);
            if (self->on_mouse_button)
            {
                self->on_mouse_button(event);
            }
        });
    }

    void LuxWindow::subscribeWindowSizeChangeCallback()
    {
        glfwSetWindowSizeCallback(_glfw_window, [](GLFWwindow* window, int width, int height) {
            auto self = static_cast<LuxWindow*>(glfwGetWindowUserPointer(window));
            if (self->on_resize)
            {
                self->on_resize(WindowResizeEvent{static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)}
                );
            }
        });
    }

    void LuxWindow::subscribeFramebufferSizeChangeCallback()
    {
        glfwSetFramebufferSizeCallback(_glfw_window, [](GLFWwindow* window, int width, int height) {
            auto self = static_cast<LuxWindow*>(glfwGetWindowUserPointer(window));
            if (self->on_framebuffer_resize)
            {
                self->on_framebuffer_resize(
                    FramebufferResizeEvent{static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)}
                );
            }
        });
    }

    float LuxWindow::lastFrameDelayTime() const
    {
        return _delta_time;
    }

    void LuxWindow::framebufferSize(std::uint32_t& width, std::uint32_t& height) const
    {
        int native_width = 0;
        int native_height = 0;
        glfwGetFramebufferSize(_glfw_window, &native_width, &native_height);
        width = static_cast<std::uint32_t>(native_width);
        height = static_cast<std::uint32_t>(native_height);
    }

#ifdef __PLATFORM_WIN32__
    void* LuxWindow::win32Handle()
    {
        return (void*)glfwGetWin32Window(_glfw_window);
    }
#endif

    void* LuxWindow::nativeHandle() const noexcept
    {
#ifdef __PLATFORM_WIN32__
        return _glfw_window ? static_cast<void*>(glfwGetWin32Window(_glfw_window)) : nullptr;
#else
        return nullptr;
#endif
    }

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
            _delta_time = current_time - _last_frame_time;
            _last_frame_time = current_time;

            glfwPollEvents();

            if (on_draw_ready)
            {
                on_draw_ready(DrawReadyEvent{});
            }

            newFrame();

            if (on_draw_finished)
            {
                on_draw_finished(DrawFinishedEvent{});
            }
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

    void LuxWindow::newFrame() {}

    void LuxWindow::recordInput(VWindowInputEvent event)
    {
        if (input_sequence_ == UINT64_MAX)
            std::abort();
        const auto sequence = ++input_sequence_;
        std::visit([sequence](auto& value) noexcept { value.sequence = sequence; }, event);
        pending_input_events_.push_back(std::move(event));
    }

    std::span<const VWindowInputEvent> LuxWindow::drainInputEvents()
    {
        drained_input_events_.clear();
        drained_input_events_.swap(pending_input_events_);
        return drained_input_events_;
    }
}
