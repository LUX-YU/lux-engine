#pragma once
#include <memory>
#include <functional>
#include <cstdint>
#include <string>
#include <span>
#include <lux/engine/window/LuxWindowDefination.hpp>
#include <lux/engine/window/InputSnapshot.hpp>
#include <lux/engine/window/WindowEvents.hpp>
#include <lux/engine/window/visibility.h>

struct GLFWwindow;

// Vulkan handle forward declarations — keeps <vulkan/vulkan.h> out of this
// public header. Matches the vulkan.h typedefs on 64-bit platforms, where
// non-dispatchable handles (VkSurfaceKHR) are pointer types too; the engine
// only targets 64-bit (x64 / arm64), enforced below.
typedef struct VkInstance_T*   VkInstance;
typedef struct VkSurfaceKHR_T* VkSurfaceKHR;
struct VkAllocationCallbacks;
static_assert(sizeof(void*) == 8,
              "LuxWindow's Vulkan handle forward declarations assume a 64-bit platform");

namespace lux::window
{
    struct InitParameter
    {
        int         width;
        int         height;
        std::string title;
    };

    enum class Mode
    {
        Headless,
        Fullscreen,
        FullscreenBorderless,
        FullscreenStretch,
        Default
    };

    enum class EWindowInitError : std::uint8_t
    {
        NONE,
        VULKAN_UNAVAILABLE,
        BACKEND_CREATE_FAILED
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

        void size(
            std::uint32_t& width,
            std::uint32_t& height
        ) const;

        bool shouldClose();

        void hideCursor(bool);

        bool setRawMouseMotion(bool enable);

        void getCursorPos(double* x, double* y) const;
        
        void setCursorPos(double x, double y);

        /// Query the instantaneous held state of a single key.
        /// Returns only KeyState::PRESS or KeyState::RELEASE — glfwGetKey()
        /// never produces REPEAT.  REPEAT events are only available through
        /// InputSnapshot::events.
        [[nodiscard]] KeyState queryKey(KeyEnum) const;

        /**
         * @brief Capture the complete input state into a value-type snapshot.
         *
         * Must be called from the main/render thread AFTER glfwPollEvents().
         * Queries all key states, mouse button states, cursor position, window
         * geometry, and drains the per-frame event queue into the result.
         *
         * The returned snapshot is suitable for transfer to the game thread via
         * TripleBuffer<InputSnapshot> — the game thread must not call
         * queryKey() / getCursorPos() etc. after this handoff.
         */
        InputSnapshot captureInputSnapshot();

        int exec();

        void setExitBehavior(EExitBehavior behavior);

        // close the window (ignore exit behavior)
        void exit();

        void hide(bool);

        /* Current version always return "glfw" */
        [[nodiscard]] std::string windowFrameworkName() const;

        float lastFrameDelayTime() const;

        void framebufferSize(
            std::uint32_t& width,
            std::uint32_t& height
        ) const;

        // ── Vulkan surface seam (backend-specific) ───────────────────
        // These two are the ONLY sanctioned way for render/ui code to get a
        // VkSurfaceKHR / the surface instance extensions. Non-backend code
        // must not touch handle() or the windowing library directly — that
        // is what keeps a future Android (ANativeWindow) backend a pure
        // source swap of this class. See
        // .internal/lux-engine-mobile-adaptation-investigation.md §3.

        /// Create a Vulkan surface for this window using the active window
        /// backend. Writes VK_NULL_HANDLE and returns false on failure.
        [[nodiscard]] bool createVulkanSurface(VkInstance instance,
                                               const VkAllocationCallbacks* allocator,
                                               VkSurfaceKHR* out_surface);

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
        template <class E>
        using EventSlot = std::function<void(const E&)>;

        EventSlot<WindowResizeEvent>       on_resize;
        EventSlot<FramebufferResizeEvent>  on_framebuffer_resize;
        EventSlot<WindowCloseEvent>        on_close;
        EventSlot<WindowFocusEvent>        on_focus;
        EventSlot<WindowLostFocusEvent>    on_lost_focus;
        EventSlot<WindowMovedEvent>        on_moved;
        EventSlot<WindowMinimizedEvent>    on_minimized;
        EventSlot<CursorEnterEvent>        on_cursor_enter;
        EventSlot<CursorLeaveEvent>        on_cursor_leave;
        EventSlot<CursorMoveEvent>         on_cursor_move;
        EventSlot<MouseButtonEvent>        on_mouse_button;
        EventSlot<MouseScrollEvent>        on_mouse_scroll;
        EventSlot<KeyEvent>                on_key;
        EventSlot<DrawReadyEvent>          on_draw_ready;
        EventSlot<DrawFinishedEvent>       on_draw_finished;
        EventSlot<FileDropEvent>           on_file_drop;

    protected:
        virtual void newFrame();

    private:

        void subscribeKeyEvent();
        
        void subscribeCursorPositionCallback();
        
        void subscribeScrollCallback();
        
        void subscribeMouseButtonCallback();

        void subscribeCharCallback();
        
        void subscribeWindowSizeChangeCallback();

        void subscribeFramebufferSizeChangeCallback();

        void subscribeDropCallback();

        static void window_close_callback(GLFWwindow* window);

        float                           _delta_time{0};
        float                           _last_frame_time{0};

        GLFWwindow*                     _glfw_window{nullptr};
        InitParameter                   _parameter;
        bool                            _init{ false };
        EWindowInitError                init_error_{EWindowInitError::NONE};
        EExitBehavior                   _exit_behavior{ EExitBehavior::EXIT };

        // --- State for captureInputSnapshot() ----------------------------
        // Callback-driven live state and per-frame edge accumulators.
        // All only accessed from the main thread.
        double                          _snapshot_cursor_x{0.0};
        double                          _snapshot_cursor_y{0.0};
        double                          _snapshot_time{0.0};
        bool                            _snapshot_initialized{false};

        static constexpr size_t         kKeyboardBitCount = 512;
        static constexpr int            kMouseButtonCount = 8;

        /// Real-time held state maintained by key callback.
        std::bitset<kKeyboardBitCount>  _live_keys_held{};

        /// Press edges accumulated by key callback since last snapshot.
        std::bitset<kKeyboardBitCount>  _pending_keys_pressed{};

        /// Release edges accumulated by key callback since last snapshot.
        std::bitset<kKeyboardBitCount>  _pending_keys_released{};

        /// Real-time held state maintained by mouse button callback.
        uint8_t                         _live_mouse_held{0};

        /// Press edges accumulated by mouse button callback since last snapshot.
        uint8_t                         _pending_mouse_pressed{0};

        /// Release edges accumulated by mouse button callback since last snapshot.
        uint8_t                         _pending_mouse_released{0};

        // Scroll delta accumulated by the GLFW scroll callback between captures.
        double                          _pending_scroll_dx{0.0};
        double                          _pending_scroll_dy{0.0};

        // Events accumulated by GLFW callbacks between captures.
        std::vector<InputEvent>         _pending_events;

        // Unicode codepoints accumulated by glfwSetCharCallback between captures.
        std::vector<CharInput>          _pending_char_inputs;
    };
} // namespace lux-engine::platform
