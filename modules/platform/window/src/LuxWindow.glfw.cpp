#include <lux/engine/window/LuxWindow.hpp>

#include <algorithm>
#include <cstdlib>
#include <thread>
#include <utility>

// Include Windows headers before GLFW to avoid APIENTRY macro redefinition warning.
// minwindef.h (pulled in by windows.h) and glfw3.h both define APIENTRY; whichever
// comes second triggers C4005.  Windows SDK headers must win.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// clang-format off: dependent Win32 headers require the umbrella definitions first.
#include <windows.h>
#include <commctrl.h>
#include <imm.h>
// clang-format on
#endif

// vulkan.h must precede glfw3.h so GLFW exposes glfwCreateWindowSurface
// (guarded by VK_VERSION_1_0). GLFW itself loads Vulkan dynamically — this
// backend needs the Vulkan headers only, not the loader library.
// clang-format off: GLFW declares its Vulkan surface API only when Vulkan is already visible.
#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>
// clang-format on
#include <cassert>
#ifdef __PLATFORM_WIN32__
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

namespace lux::window
{
    namespace
    {
        WindowRect windowRect(GLFWwindow* window) noexcept
        {
            WindowRect result;
            glfwGetWindowPos(window, &result.x, &result.y);
            glfwGetWindowSize(window, &result.width, &result.height);
            return result;
        }

        DisplayHint displayHint(GLFWmonitor* monitor)
        {
            DisplayHint result;
            const char* name = glfwGetMonitorName(monitor);
            result.name = name ? name : "";
            auto& work = result.work_area;
            glfwGetMonitorWorkarea(monitor, &work.x, &work.y, &work.width, &work.height);
            return result;
        }

        DisplayInfo displayInfo(GLFWmonitor* monitor)
        {
            DisplayInfo result;
            result.hint = displayHint(monitor);
            glfwGetMonitorPos(monitor, &result.bounds.x, &result.bounds.y);
            glfwGetMonitorContentScale(monitor, &result.scale.x, &result.scale.y);
            if (const auto* mode = glfwGetVideoMode(monitor))
            {
                result.current_mode = {mode->width, mode->height, mode->refreshRate};
                result.bounds.width = mode->width;
                result.bounds.height = mode->height;
            }
            int count{};
            const auto* modes = glfwGetVideoModes(monitor, &count);
            result.modes.reserve(static_cast<std::size_t>(std::max(0, count)));
            for (int i = 0; i < count; ++i)
            {
                result.modes.push_back({modes[i].width, modes[i].height, modes[i].refreshRate});
            }
            result.primary = monitor == glfwGetPrimaryMonitor();
            return result;
        }

        WindowPlacementFailure placementFailure(EWindowPlacementError code, const char* message)
        {
            return {code, message};
        }
    } // namespace

    struct LuxWindow::NativeWindowState final
    {
        GLFWwindow* window{};
        InitParameter parameter;

        ~NativeWindowState() noexcept
        {
            if (window)
            {
                // IME teardown must not access a failed candidate or a destructing owner.
                glfwSetWindowUserPointer(window, nullptr);
                glfwDestroyWindow(window);
            }
        }
    };

    void LuxWindow::NativeWindowDeleter::operator()(NativeWindowState* state) const noexcept
    {
        delete state;
    }

    LuxWindow::DisplaysResult LuxWindow::displays() noexcept
    {
        int count{};
        const auto* monitors = glfwGetMonitors(&count);
        if (!monitors)
        {
            return lux::cxx::unexpected(
                placementFailure(EWindowPlacementError::NO_DISPLAY, "No initialized desktop displays")
            );
        }
        std::vector<DisplayInfo> result;
        result.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i)
        {
            result.push_back(displayInfo(monitors[i]));
        }
        return result;
    }

    LuxWindow::StateResult LuxWindow::state() const noexcept
    {
        WindowState result;
        result.content = windowRect(native_->window);
        result.placement.normal = normal_rect_;
        auto* monitor = glfwGetWindowMonitor(native_->window);
        result.placement.mode = monitor
                                    ? EWindowMode::FULLSCREEN
                                    : (glfwGetWindowAttrib(native_->window, GLFW_MAXIMIZED) ? EWindowMode::MAXIMIZED
                                                                                            : EWindowMode::ORDINARY);
        glfwGetWindowContentScale(native_->window, &result.scale.x, &result.scale.y);
        auto& insets = result.insets;
        glfwGetWindowFrameSize(native_->window, &insets.left, &insets.top, &insets.right, &insets.bottom);
        result.minimized = minimized();
        if (monitor)
        {
            result.placement.display = displayHint(monitor);
        }
        else
        {
            int count{};
            auto* const* monitors = glfwGetMonitors(&count);
            if (!monitors)
            {
                return lux::cxx::unexpected(
                    placementFailure(EWindowPlacementError::NO_DISPLAY, "Window has no available desktop display")
                );
            }
            std::int64_t best_area{-1};
            for (int i = 0; i < count; ++i)
            {
                auto hint = displayHint(monitors[i]);
                const auto& area = hint.work_area;
                const auto& rect = result.content;
                const auto width = std::max<std::int64_t>(
                    0,
                    std::min(std::int64_t{rect.x} + rect.width, std::int64_t{area.x} + area.width) -
                        std::max(rect.x, area.x)
                );
                const auto height = std::max<std::int64_t>(
                    0,
                    std::min(std::int64_t{rect.y} + rect.height, std::int64_t{area.y} + area.height) -
                        std::max(rect.y, area.y)
                );
                if (width * height > best_area)
                {
                    best_area = width * height;
                    result.placement.display = std::move(hint);
                }
            }
        }
        return result;
    }

    LuxWindow::StateResult LuxWindow::applyPlacement(const WindowPlacement& request) noexcept
    {
        auto available = displays();
        if (!available)
        {
            return lux::cxx::unexpected(available.error());
        }
        WindowInsets insets;
        if (!glfwGetWindowMonitor(native_->window))
        {
            glfwGetWindowFrameSize(native_->window, &insets.left, &insets.top, &insets.right, &insets.bottom);
        }
        // Saved values may be repaired by policy. Explicit mode and size must be valid.
        auto resolved = resolveWindowPlacement(
            {request, WindowSize{request.normal.width, request.normal.height}, request.mode, request.display},
            *available,
            insets
        );
        if (!resolved)
        {
            return lux::cxx::unexpected(resolved.error());
        }
        const auto& placement = resolved->placement;
        int count{};
        auto* const* monitors = glfwGetMonitors(&count);
        GLFWmonitor* target{};
        for (int i = 0; i < count; ++i)
        {
            const auto current = displayHint(monitors[i]);
            const bool is_match =
                current.name == placement.display.name && current.work_area == placement.display.work_area;
            if (is_match)
            {
                target = monitors[i];
                break;
            }
        }
        if (!target)
        {
            return lux::cxx::unexpected(
                placementFailure(EWindowPlacementError::NO_DISPLAY, "Selected display was disconnected")
            );
        }
        const auto* mode = glfwGetVideoMode(target);
        if (!mode)
        {
            return lux::cxx::unexpected(
                placementFailure(EWindowPlacementError::UNSUPPORTED, "Selected display has no current mode")
            );
        }
        glfwGetError(nullptr);
        changing_placement_ = true;
        glfwRestoreWindow(native_->window);
        normal_rect_ = placement.normal;
        if (placement.mode == EWindowMode::FULLSCREEN)
        {
            glfwSetWindowMonitor(native_->window, target, 0, 0, mode->width, mode->height, mode->refreshRate);
        }
        else
        {
            const auto& rect = placement.normal;
            glfwSetWindowMonitor(native_->window, nullptr, rect.x, rect.y, rect.width, rect.height, GLFW_DONT_CARE);
            if (placement.mode == EWindowMode::MAXIMIZED)
            {
                glfwMaximizeWindow(native_->window);
            }
        }
        changing_placement_ = false;
        const char* message{};
        const int error = glfwGetError(&message);
        placementChanged();
        if (error != GLFW_NO_ERROR)
        {
            return lux::cxx::unexpected(placementFailure(
                EWindowPlacementError::PLATFORM,
                message ? message : "Platform rejected window placement"
            ));
        }
        return state();
    }

    void LuxWindow::placementChanged() noexcept
    {
        if (changing_placement_)
        {
            return;
        }
        const bool is_normal = !glfwGetWindowMonitor(native_->window) &&
                               !glfwGetWindowAttrib(native_->window, GLFW_MAXIMIZED) && !minimized();
        if (is_normal)
        {
            normal_rect_ = windowRect(native_->window);
        }
        if (on_placement_changed)
        {
            on_placement_changed({});
        }
    }

    void LuxWindow::windowCloseCallback(GLFWwindow* window)
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

        if (window_impl->exit_behavior_ == EExitBehavior::EXIT)
        {
            return;
        }
        else if (window_impl->exit_behavior_ == EExitBehavior::HIDE)
        {
            glfwSetWindowShouldClose(window, false);
            glfwHideWindow(window);
        }
    }

    bool LuxWindow::focused() const noexcept
    {
        return glfwGetWindowAttrib(native_->window, GLFW_FOCUSED);
    }

    bool LuxWindow::visible() const noexcept
    {
        return glfwGetWindowAttrib(native_->window, GLFW_VISIBLE);
    }

    bool LuxWindow::minimized() const noexcept
    {
        return glfwGetWindowAttrib(native_->window, GLFW_ICONIFIED);
    }

    void LuxWindow::waitEvents(double timeout_seconds)
    {
        if (timeout_seconds > 0.0)
        {
            glfwWaitEventsTimeout(timeout_seconds);
        }
        else
        {
            glfwPollEvents();
        }
    }

    void LuxWindow::wakeEvents() noexcept
    {
        glfwPostEmptyEvent();
    }

    LuxWindow::CreateResult LuxWindow::create(InitParameter parameter) noexcept
    {
        auto native = prepareNative(std::move(parameter));
        if (!native)
        {
            return lux::cxx::unexpected(native.error());
        }
        return std::unique_ptr<LuxWindow>(new LuxWindow(std::move(*native)));
    }

    LuxWindow::NativeWindowResult LuxWindow::prepareNative(InitParameter parameter) noexcept
    {
        if (!glfwVulkanSupported())
        {
            return lux::cxx::unexpected(EWindowInitError::VULKAN_UNAVAILABLE);
        }
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        auto native = NativeWindowOwner{new NativeWindowState{nullptr, std::move(parameter)}};
        const auto& config = native->parameter;
        native->window = glfwCreateWindow(config.width, config.height, config.title.c_str(), nullptr, nullptr);
        if (!native->window)
        {
            return lux::cxx::unexpected(EWindowInitError::BACKEND_CREATE_FAILED);
        }

#if defined(_WIN32)
        const auto capture_ime =
            [](HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR context) -> LRESULT
        {
            if (message == WM_NCDESTROY)
            {
                RemoveWindowSubclass(window, reinterpret_cast<SUBCLASSPROC>(id), id);
                return DefSubclassProc(window, message, wparam, lparam);
            }
            auto* native = reinterpret_cast<GLFWwindow*>(context);
            auto* owner = static_cast<LuxWindow*>(glfwGetWindowUserPointer(native));
            if (!owner)
            {
                return DefSubclassProc(window, message, wparam, lparam);
            }
            switch (message)
            {
            case WM_IME_STARTCOMPOSITION:
                owner->composing_ = true;
                owner->recordInput(WindowCompositionEvent{ECompositionStage::STARTED});
                break;
            case WM_IME_COMPOSITION:
                if (lparam & GCS_RESULTSTR)
                {
                    owner->composing_ = false;
                    owner->recordInput(WindowCompositionEvent{ECompositionStage::COMMITTED});
                }
                if (lparam & (GCS_COMPSTR | GCS_COMPATTR | GCS_COMPCLAUSE | GCS_CURSORPOS))
                {
                    owner->composing_ = true;
                    owner->recordInput(WindowCompositionEvent{ECompositionStage::UPDATED});
                }
                if (!lparam && std::exchange(owner->composing_, false))
                {
                    owner->recordInput(WindowCompositionEvent{ECompositionStage::CANCELLED});
                }
                break;
            case WM_KILLFOCUS:
            case WM_IME_ENDCOMPOSITION:
                if (std::exchange(owner->composing_, false))
                {
                    owner->recordInput(WindowCompositionEvent{ECompositionStage::CANCELLED});
                }
                break;
            }
            // Do not read/submit GCS_RESULTSTR. GLFW's character callback remains
            // the sole committed-text path; Windows owns composition/candidates.
            return DefSubclassProc(window, message, wparam, lparam);
        };
        const auto procedure = static_cast<SUBCLASSPROC>(capture_ime);
        if (!SetWindowSubclass(
                glfwGetWin32Window(native->window),
                procedure,
                reinterpret_cast<UINT_PTR>(procedure),
                reinterpret_cast<DWORD_PTR>(native->window)
            ))
        {
            return lux::cxx::unexpected(EWindowInitError::CALLBACK_REGISTRATION_FAILED);
        }
#endif
        return native;
    }

    LuxWindow::LuxWindow(NativeWindowOwner native) noexcept : native_(std::move(native))
    {
        glfwSetWindowUserPointer(native_->window, this);
        glfwSetWindowCloseCallback(native_->window, &LuxWindow::windowCloseCallback);
        normal_rect_ = windowRect(native_->window);
        glfwSetWindowPosCallback(
            native_->window,
            [](GLFWwindow* window, int x, int y)
            {
                auto& self = *static_cast<LuxWindow*>(glfwGetWindowUserPointer(window));
                self.placementChanged();
                if (self.on_moved)
                {
                    self.on_moved({x, y});
                }
            }
        );
        glfwSetWindowMaximizeCallback(
            native_->window,
            [](GLFWwindow* window, int)
            { static_cast<LuxWindow*>(glfwGetWindowUserPointer(window))->placementChanged(); }
        );
        glfwSetWindowIconifyCallback(
            native_->window,
            [](GLFWwindow* window, int minimized)
            {
                auto& self = *static_cast<LuxWindow*>(glfwGetWindowUserPointer(window));
                self.placementChanged();
                if (self.on_minimized)
                {
                    self.on_minimized({minimized == GLFW_TRUE});
                }
            }
        );
        glfwSetWindowContentScaleCallback(
            native_->window,
            [](GLFWwindow* window, float, float)
            { static_cast<LuxWindow*>(glfwGetWindowUserPointer(window))->placementChanged(); }
        );
        glfwSetWindowFocusCallback(
            native_->window,
            [](GLFWwindow* window, int focused)
            {
                auto* self = static_cast<LuxWindow*>(glfwGetWindowUserPointer(window));
                if (focused)
                {
                    self->recordInput(WindowFocusEvent{});
                    if (self->on_focus)
                    {
                        self->on_focus({});
                    }
                }
                else
                {
                    if (std::exchange(self->composing_, false))
                    {
                        self->recordInput(WindowCompositionEvent{ECompositionStage::CANCELLED});
                    }
                    self->recordInput(WindowLostFocusEvent{});
                    if (self->on_lost_focus)
                    {
                        self->on_lost_focus({});
                    }
                }
            }
        );

        // Enable CapsLock / NumLock modifier bits in key/mouse callbacks.
        glfwSetInputMode(native_->window, GLFW_LOCK_KEY_MODS, GLFW_TRUE);

        subscribeKeyEvent();
        subscribeCursorPositionCallback();
        subscribeScrollCallback();
        subscribeMouseButtonCallback();
        subscribeCharCallback();
        subscribeWindowSizeChangeCallback();
        subscribeFramebufferSizeChangeCallback();
        subscribeDropCallback();
    }

    LuxWindow::~LuxWindow() noexcept = default;

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
        const VkResult result = glfwCreateWindowSurface(instance, native_->window, allocator, out_surface);
        if (result != VK_SUCCESS || *out_surface == VK_NULL_HANDLE)
        {
            *out_surface = VK_NULL_HANDLE;
            return false;
        }
        return true;
    }

    std::span<const char* const> LuxWindow::requiredVulkanInstanceExtensions()
    {
        uint32_t count = 0;
        const char** extensions = glfwGetRequiredInstanceExtensions(&count);
        if (extensions == nullptr)
        {
            return {};
        }
        return {extensions, count};
    }

    const char* LuxWindow::title() const
    {
        return native_->parameter.title.c_str();
    }

    bool LuxWindow::shouldClose()
    {
        return glfwWindowShouldClose(native_->window);
    }

    void LuxWindow::hideCursor(bool enable)
    {
        glfwSetInputMode(native_->window, GLFW_CURSOR, enable ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    }

    bool LuxWindow::setRawMouseMotion(bool enable)
    {
        if (!glfwRawMouseMotionSupported())
        {
            return false;
        }
        glfwSetInputMode(native_->window, GLFW_RAW_MOUSE_MOTION, enable ? GLFW_TRUE : GLFW_FALSE);
        return true;
    }

    void LuxWindow::getCursorPos(double* x, double* y) const
    {
        glfwGetCursorPos(native_->window, x, y);
    }

    void LuxWindow::setCursorPos(double x, double y)
    {
        glfwSetCursorPos(native_->window, x, y);
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
        glfwGetWindowSize(native_->window, &native_width, &native_height);
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
        glfwSetKeyCallback(
            native_->window,
            [](GLFWwindow* window, int key, int scancode, int action, int mods)
            {
                auto self = static_cast<LuxWindow*>(glfwGetWindowUserPointer(window));
                WindowKeyEvent event{key, scancode, action, mods};
                self->recordInput(event);
                if (self->on_key)
                {
                    self->on_key(event);
                }
            }
        );
    }

    void LuxWindow::subscribeCursorPositionCallback()
    {
        glfwSetCursorPosCallback(
            native_->window,
            [](GLFWwindow* window, double xpos, double ypos)
            {
                auto self = static_cast<LuxWindow*>(glfwGetWindowUserPointer(window));
                self->recordInput(CursorMoveEvent{xpos, ypos});
                if (self->on_cursor_move)
                {
                    self->on_cursor_move(CursorMoveEvent{xpos, ypos});
                }
            }
        );
    }

    void LuxWindow::subscribeScrollCallback()
    {
        glfwSetScrollCallback(
            native_->window,
            [](GLFWwindow* window, double xoffset, double yoffset)
            {
                auto self = static_cast<LuxWindow*>(glfwGetWindowUserPointer(window));
                const WindowScrollEvent event{xoffset, yoffset};
                self->recordInput(event);
                if (self->on_mouse_scroll)
                {
                    self->on_mouse_scroll(event);
                }
            }
        );
    }

    void LuxWindow::subscribeDropCallback()
    {
        glfwSetDropCallback(
            native_->window,
            [](GLFWwindow* window, int count, const char** paths)
            {
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
            }
        );
    }

    void LuxWindow::subscribeCharCallback()
    {
        glfwSetCharCallback(
            native_->window,
            [](GLFWwindow* window, unsigned int codepoint)
            {
                auto self = static_cast<LuxWindow*>(glfwGetWindowUserPointer(window));
                self->recordInput(WindowTextEvent{.codepoint = codepoint});
            }
        );
    }

    void LuxWindow::subscribeMouseButtonCallback()
    {
        glfwSetMouseButtonCallback(
            native_->window,
            [](GLFWwindow* window, int button, int action, int mods)
            {
                auto self = static_cast<LuxWindow*>(glfwGetWindowUserPointer(window));

                WindowMouseButtonEvent event{button, action, mods};
                self->recordInput(event);
                if (self->on_mouse_button)
                {
                    self->on_mouse_button(event);
                }
            }
        );
    }

    void LuxWindow::subscribeWindowSizeChangeCallback()
    {
        glfwSetWindowSizeCallback(
            native_->window,
            [](GLFWwindow* window, int width, int height)
            {
                auto self = static_cast<LuxWindow*>(glfwGetWindowUserPointer(window));
                self->placementChanged();
                if (self->on_resize)
                {
                    self->on_resize(
                        WindowResizeEvent{static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)}
                    );
                }
            }
        );
    }

    void LuxWindow::subscribeFramebufferSizeChangeCallback()
    {
        glfwSetFramebufferSizeCallback(
            native_->window,
            [](GLFWwindow* window, int width, int height)
            {
                auto self = static_cast<LuxWindow*>(glfwGetWindowUserPointer(window));
                if (self->on_framebuffer_resize)
                {
                    self->on_framebuffer_resize(
                        FramebufferResizeEvent{static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)}
                    );
                }
            }
        );
    }

    float LuxWindow::lastFrameDelayTime() const
    {
        return delta_time_;
    }

    void LuxWindow::framebufferSize(std::uint32_t& width, std::uint32_t& height) const
    {
        int native_width = 0;
        int native_height = 0;
        glfwGetFramebufferSize(native_->window, &native_width, &native_height);
        width = static_cast<std::uint32_t>(native_width);
        height = static_cast<std::uint32_t>(native_height);
    }

#ifdef __PLATFORM_WIN32__
    void* LuxWindow::win32Handle()
    {
        return (void*)glfwGetWin32Window(native_->window);
    }
#endif

    void* LuxWindow::nativeHandle() const noexcept
    {
#ifdef __PLATFORM_WIN32__
        return native_->window ? static_cast<void*>(glfwGetWin32Window(native_->window)) : nullptr;
#else
        return nullptr;
#endif
    }

    GLFWwindow* LuxWindow::handle()
    {
        return native_->window;
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
        while (!glfwWindowShouldClose(native_->window))
        {
            float current_time = timeAfterFirstInitialization();
            delta_time_ = current_time - last_frame_time_;
            last_frame_time_ = current_time;

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
        exit_behavior_ = EExitBehavior::EXIT;
        glfwSetWindowShouldClose(native_->window, GLFW_TRUE);
    }

    void LuxWindow::hide(bool var)
    {
        var ? glfwHideWindow(native_->window) : glfwShowWindow(native_->window);
    }

    void LuxWindow::setExitBehavior(EExitBehavior behavior)
    {
        exit_behavior_ = behavior;
    }

    void LuxWindow::newFrame() {}

    void LuxWindow::recordInput(VWindowInputEvent event)
    {
        if (input_sequence_ == UINT64_MAX)
        {
            std::abort();
        }
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
} // namespace lux::window
