#pragma once
#include <memory>
#include <functional>
#include <cstdint>
#include <string>
#include <span>
#include <vector>
#include <lux/engine/window/WindowEvents.hpp>
#include <lux/engine/window/WindowPlacement.hpp>
#include <lux/engine/window/visibility.h>

struct GLFWwindow;

// Vulkan handle forward declarations — keeps <vulkan/vulkan.h> out of this
// public header. Matches the vulkan.h typedefs on 64-bit platforms, where
// non-dispatchable handles (VkSurfaceKHR) are pointer types too; the engine
// only targets 64-bit (x64 / arm64), enforced below.
typedef struct VkInstance_T* VkInstance;
typedef struct VkSurfaceKHR_T* VkSurfaceKHR;
struct VkAllocationCallbacks;
static_assert(sizeof(void*) == 8, "LuxWindow's Vulkan handle forward declarations assume a 64-bit platform");

namespace lux::window
{
    struct InitParameter
    {
        int width;
        int height;
        std::string title;
    };

    enum class EWindowInitError : std::uint8_t
    {
        NONE,
        VULKAN_UNAVAILABLE,
        BACKEND_CREATE_FAILED
    };

    enum class EExitBehavior
    {
        EXIT,
        HIDE
    };

    class LuxWindow;

    class LUX_PLATFORM_WINDOW_PUBLIC LuxWindow
    {
    public:
        /**
         * @brief Every constructor runs init() and records its result.
         *
         * A constructor cannot report failure, so the caller MUST check
         * isInitialized() before using the window — a non-initialized
         * LuxWindow has no native handle and every operation on it is a
         * no-op. GLFW must already be up (see GlfwRuntime).
         */
        LuxWindow(int width, int height, std::string title);

        explicit LuxWindow(const InitParameter& parameter);

        /**
         * @brief Idempotent; already run by the constructors. Public only so a
         *        window whose first bring-up failed can be retried in place.
         *        Failure is available through initError(); the platform layer
         *        never chooses a logging sink.
         */
        [[nodiscard]] virtual bool init();

        [[nodiscard]] bool isInitialized() const;

        [[nodiscard]] EWindowInitError initError() const noexcept
        {
            return init_error_;
        }

        virtual ~LuxWindow();

        [[nodiscard]] const char* title() const;

        void size(std::uint32_t& width, std::uint32_t& height) const;

        bool shouldClose();

        [[nodiscard]] bool focused() const noexcept;
        [[nodiscard]] bool visible() const noexcept;
        [[nodiscard]] bool minimized() const noexcept;

        // Owner-thread snapshots. Applying returns observed platform facts, not
        // a promise that the OS adopted every requested position/size exactly.
        using DisplaysResult = lux::cxx::expected<std::vector<DisplayInfo>, WindowPlacementFailure>;
        using StateResult = lux::cxx::expected<WindowState, WindowPlacementFailure>;
        [[nodiscard]] static DisplaysResult displays() noexcept;
        [[nodiscard]] StateResult state() const noexcept;
        [[nodiscard]] StateResult applyPlacement(const WindowPlacement&) noexcept;

        // Platform-adapter borrow; valid only during this window's lifetime.
        [[nodiscard]] void* nativeHandle() const noexcept;

        void hideCursor(bool);

        bool setRawMouseMotion(bool enable);

        void getCursorPos(double* x, double* y) const;

        void setCursorPos(double x, double y);

        /// Borrow the ordered native input batch until the next drain. Further
        /// callbacks append to a different buffer. Window owner thread only.
        [[nodiscard]] std::span<const VWindowInputEvent> drainInputEvents();

        int exec();

        void setExitBehavior(EExitBehavior behavior);

        // close the window (ignore exit behavior)
        void exit();

        void hide(bool);

        /* Current version always return "glfw" */
        [[nodiscard]] std::string windowFrameworkName() const;

        float lastFrameDelayTime() const;

        void framebufferSize(std::uint32_t& width, std::uint32_t& height) const;

        // ── Vulkan surface seam (backend-specific) ───────────────────
        // These two are the ONLY sanctioned way for render/ui code to get a
        // VkSurfaceKHR / the surface instance extensions. Non-backend code
        // must not touch handle() or the windowing library directly — that
        // is what keeps a future Android (ANativeWindow) backend a pure
        // source swap of this class. See
        // the implementation §3.

        /// Create a Vulkan surface for this window using the active window
        /// backend. Writes VK_NULL_HANDLE and returns false on failure.
        [[nodiscard]] bool createVulkanSurface(
            VkInstance instance,
            const VkAllocationCallbacks* allocator,
            VkSurfaceKHR* out_surface
        );

        /// Vulkan instance extensions the window backend needs for surface
        /// creation (e.g. VK_KHR_surface + the platform surface extension).
        /// Idempotently initializes the backend runtime so it is valid to
        /// call before any window exists. The pointed-to strings have static
        /// lifetime (owned by the backend); empty on failure.
        [[nodiscard]] static std::span<const char* const> requiredVulkanInstanceExtensions();

        // On Android there is deliberately NO way to hand a native window to
        // this class. The OS's window arrives at APP_CMD_INIT_WINDOW and is
        // taken back at APP_CMD_TERM_WINDOW, repeatedly within one session —
        // its lifetime matches a SURFACE, not a window object. Routing it
        // through here would tie every consumer of LuxWindow to that cycle.
        //
        // The handle goes straight to the surface instead, as a POD payload on
        // the CreateSurfaceTarget command: see RenderSurface::initFromNative.

#ifdef __PLATFORM_WIN32__
        // Get windows
        void* win32Handle();
#endif
        static void pollEvents();

        static void waitEvents();
        static void waitEvents(double timeout_seconds);
        // Thread-safe wake of the native event wait. The platform runtime must remain alive.
        static void wakeEvents() noexcept;

        static double timeAfterFirstInitialization();

        // Get glfw context
        GLFWwindow* handle();

        static GLFWwindow* currentContext();

        static void makeContextCurrent(GLFWwindow*);

        using ProcPtr = void (*)();
        static ProcPtr getProcAddress(const char* name);

        bool vulkanSupported();

        // ── 回调缝(单槽)────────────────────────────────────
        // 基础模块零事件概念(统一事件系统裁决③):每种窗口事实一个
        // std::function 槽,装配层设置一次(`window.on_xxx = handler`),
        // 置空即断开。**不做订阅表** —— Signal 本身就是一个小事件系统,
        // 与统一总线冗余;真要扇出,装配层把回调翻译成总线事件(批G 的
        // 窗口域事件正是这形状)。回调在 pollEvents 的线程(主线程)上跑。
        template <class E> using EventSlot = std::function<void(const E&)>;

        EventSlot<WindowResizeEvent> on_resize;
        EventSlot<FramebufferResizeEvent> on_framebuffer_resize;
        EventSlot<WindowCloseEvent> on_close;
        EventSlot<WindowFocusEvent> on_focus;
        EventSlot<WindowLostFocusEvent> on_lost_focus;
        EventSlot<WindowMovedEvent> on_moved;
        EventSlot<WindowMinimizedEvent> on_minimized;
        // Coalescible fact notification; query state at the host's safe point.
        EventSlot<WindowPlacementEvent> on_placement_changed;
        EventSlot<CursorEnterEvent> on_cursor_enter;
        EventSlot<CursorLeaveEvent> on_cursor_leave;
        EventSlot<CursorMoveEvent> on_cursor_move;
        EventSlot<WindowMouseButtonEvent> on_mouse_button;
        EventSlot<WindowScrollEvent> on_mouse_scroll;
        EventSlot<WindowKeyEvent> on_key;
        EventSlot<DrawReadyEvent> on_draw_ready;
        EventSlot<DrawFinishedEvent> on_draw_finished;
        EventSlot<FileDropEvent> on_file_drop;

    protected:
        virtual void newFrame();

    private:
        void recordInput(VWindowInputEvent);
        void placementChanged() noexcept;
        void subscribeKeyEvent();

        void subscribeCursorPositionCallback();

        void subscribeScrollCallback();

        void subscribeMouseButtonCallback();

        void subscribeCharCallback();

        void subscribeWindowSizeChangeCallback();

        void subscribeFramebufferSizeChangeCallback();

        void subscribeDropCallback();

        static void window_close_callback(GLFWwindow* window);

        float _delta_time{0};
        float _last_frame_time{0};

        GLFWwindow* _glfw_window{nullptr};
        InitParameter _parameter;
        bool _init{false};
        EWindowInitError init_error_{EWindowInitError::NONE};
        EExitBehavior _exit_behavior{EExitBehavior::EXIT};

        std::uint64_t input_sequence_{};
        bool composing_{};
        WindowRect normal_rect_;
        bool changing_placement_{};
        std::vector<VWindowInputEvent> pending_input_events_;
        std::vector<VWindowInputEvent> drained_input_events_;
    };
} // namespace lux-engine::platform
