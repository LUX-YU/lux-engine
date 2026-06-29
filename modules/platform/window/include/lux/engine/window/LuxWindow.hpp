#pragma once
#include <memory>
#include <functional>
#include <string>
#include <lux/engine/window/LuxWindowDefination.hpp>
#include <lux/engine/window/InputSnapshot.hpp>
#include <lux/engine/window/WindowEvents.hpp>
#include <lux/cxx/event/Signal.hpp>
#include <lux/engine/window/visibility.h>

struct GLFWwindow;

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

    class LuxWindow;

    class LUX_PLATFORM_WINDOW_PUBLIC LuxWindow
    {
    public:
        /**
         * @brief init() won't be called automaticly
        */          
        LuxWindow(int width, int height, std::string title);

        LuxWindow(common::Size2D size, std::string title);

        explicit LuxWindow(const InitParameter& parameter);

        /**
         * @brief call after calling bindContext
        */
        [[nodiscard]] virtual bool init();

        [[nodiscard]] bool isInitialized() const;

        virtual ~LuxWindow();

        [[nodiscard]] const char* title() const;

        [[nodiscard]] common::Size2D size() const;

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

        common::Size2D framebufferSize() const;

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

        // ── Signals ──────────────────────────────────────────
        lux::cxx::event::Signal<WindowResizeEvent>       on_resize;
        lux::cxx::event::Signal<FramebufferResizeEvent>  on_framebuffer_resize;
        lux::cxx::event::Signal<WindowCloseEvent>        on_close;
        lux::cxx::event::Signal<WindowFocusEvent>        on_focus;
        lux::cxx::event::Signal<WindowLostFocusEvent>    on_lost_focus;
        lux::cxx::event::Signal<WindowMovedEvent>        on_moved;
        lux::cxx::event::Signal<WindowMinimizedEvent>    on_minimized;
        lux::cxx::event::Signal<CursorEnterEvent>        on_cursor_enter;
        lux::cxx::event::Signal<CursorLeaveEvent>        on_cursor_leave;
        lux::cxx::event::Signal<CursorMoveEvent>         on_cursor_move;
        lux::cxx::event::Signal<MouseButtonEvent>        on_mouse_button;
        lux::cxx::event::Signal<MouseScrollEvent>        on_mouse_scroll;
        lux::cxx::event::Signal<KeyEvent>                on_key;
        lux::cxx::event::Signal<DrawReadyEvent>          on_draw_ready;
        lux::cxx::event::Signal<DrawFinishedEvent>       on_draw_finished;
        lux::cxx::event::Signal<FileDropEvent>          on_file_drop;

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
