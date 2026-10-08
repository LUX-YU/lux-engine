#pragma once
#include <cstdint>
#include <functional>
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/window/WindowEvents.hpp>
#include <lux/engine/window/WindowPlacement.hpp>
#include <lux/engine/window/visibility.h>
#include <memory>
#include <span>
#include <string>
#include <vector>

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
        VULKAN_UNAVAILABLE = 1,
        BACKEND_CREATE_FAILED,
        CALLBACK_REGISTRATION_FAILED,
        UNSUPPORTED_PLATFORM
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
        using CreateResult = lux::cxx::expected<std::unique_ptr<LuxWindow>, EWindowInitError>;
        // The platform runtime must outlive this complete native-window owner.
        [[nodiscard]] static CreateResult create(InitParameter) noexcept;
        virtual ~LuxWindow() noexcept;

        LuxWindow(const LuxWindow&) = delete;
        LuxWindow& operator=(const LuxWindow&) = delete;
        LuxWindow(LuxWindow&&) = delete;
        LuxWindow& operator=(LuxWindow&&) = delete;

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
        /// Requires an existing platform runtime, but not a window. Strings
        /// are borrowed until runtime destruction; empty on backend failure.
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
        struct NativeWindowState;

        struct NativeWindowDeleter final
        {
            LUX_PLATFORM_WINDOW_PUBLIC void operator()(NativeWindowState*) const noexcept;
        };

        using NativeWindowOwner = std::unique_ptr<NativeWindowState, NativeWindowDeleter>;
        using NativeWindowResult = lux::cxx::expected<NativeWindowOwner, EWindowInitError>;
        // Derived factories prepare all fallible native work before publishing their owner.
        [[nodiscard]] static NativeWindowResult prepareNative(InitParameter) noexcept;
        explicit LuxWindow(NativeWindowOwner) noexcept;
        virtual void newFrame();

    private:
        friend class TrayIcon;
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

        static void windowCloseCallback(GLFWwindow* window);

        float delta_time_{0};
        float last_frame_time_{0};

        NativeWindowOwner native_;
        EExitBehavior exit_behavior_{EExitBehavior::EXIT};

        std::uint64_t input_sequence_{};
        bool composing_{};
        WindowRect normal_rect_;
        bool changing_placement_{};
        std::vector<VWindowInputEvent> pending_input_events_;
        std::vector<VWindowInputEvent> drained_input_events_;
    };
} // namespace lux::window
