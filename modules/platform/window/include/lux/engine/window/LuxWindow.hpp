#pragma once
#include <memory>
#include <functional>
#include <string>
#include <lux/engine/window/LuxWindowDefination.hpp>
#include <lux/engine/event/Event.hpp>
#include <lux/engine/platform/visibility.h>
#include "GraphicContext.hpp"
#include "ContextVisitor.hpp"

struct GLFWwindow;

namespace lux::window
{
    struct InitParameter
    {
        int         width;
        int         height;
        std::string title;
    };

    class LuxWindow;

    enum class EWindowEvents
    {
        WindowSizeChanged,
        WindowClose,
        WindowFocus,
        WindowLostFocus,
        WindowMoved,
        WindowMinimized,
        CursorEnter,
        CursorLeave,
        CursorPositionChanged,
        MouseButtonAction,
        MouseScrollAction,
        Key,
        DrawReady,
        DrawFinished
    };

    using WindowSizeChangedEvent    = lux::event::TEvent<WindowSize, static_cast<uint64_t>(EWindowEvents::WindowSizeChanged)>;
    using WindowCloseEvent          = lux::event::TEvent<void, static_cast<uint64_t>(EWindowEvents::WindowClose)>;
    using WindowFocusEvent          = lux::event::TEvent<void, static_cast<uint64_t>(EWindowEvents::WindowFocus)>;
    using WindowLostFocusEvent      = lux::event::TEvent<void, static_cast<uint64_t>(EWindowEvents::WindowLostFocus)>;
    using WindowMovedEvent          = lux::event::TEvent<void, static_cast<uint64_t>(EWindowEvents::WindowMoved)>;
    using WindowMinimizedEvent      = lux::event::TEvent<void, static_cast<uint64_t>(EWindowEvents::WindowMinimized)>;
    using CursorEnterEvent          = lux::event::TEvent<void, static_cast<uint64_t>(EWindowEvents::CursorEnter)>;
    using CursorLeaveEvent          = lux::event::TEvent<void, static_cast<uint64_t>(EWindowEvents::CursorLeave)>;
    using CursorPositionChangedEvent= lux::event::TEvent<CursorPositionChanged, static_cast<uint64_t>(EWindowEvents::CursorPositionChanged)>;
    using MouseButtonEvent          = lux::event::TEvent<MouseButtonAction, static_cast<uint64_t>(EWindowEvents::MouseButtonAction)>;
    using MouseScrollEvent          = lux::event::TEvent<MouseScrollAction, static_cast<uint64_t>(EWindowEvents::MouseScrollAction)>;
    using KeyEvent                  = lux::event::TEvent<KeyAction, static_cast<uint64_t>(EWindowEvents::Key)>;
    using DrawReadyEvent            = lux::event::TEvent<void, static_cast<uint64_t>(EWindowEvents::DrawReady)>;
    using DrawFinishedEvent         = lux::event::TEvent<void, static_cast<uint64_t>(EWindowEvents::DrawFinished)>;

    using WindowEventDispatcher     = lux::event::EventDispatcher<
        WindowSizeChangedEvent,     WindowCloseEvent,
        WindowFocusEvent,           WindowLostFocusEvent,
        WindowMovedEvent,           WindowMinimizedEvent,
        CursorEnterEvent,           CursorLeaveEvent,
        CursorPositionChangedEvent, MouseButtonEvent,
        MouseScrollEvent,           KeyEvent,
        DrawReadyEvent,             DrawFinishedEvent
    >;

    using SharedContextPtr = std::shared_ptr<GraphicContext>;

    template<class T, class... Args> SharedContextPtr createContext(Args&&... args)
    requires std::is_base_of_v<GraphicContext, T>
    {
        return std::make_shared<T>(std::forward<Args>(args)...);
    }

    class LuxWindow : protected ContextVisitor, public WindowEventDispatcher
    {
    public:
        /**
         * @brief init() will be called automaticly
        */
        LUX_PLATFORM_PUBLIC  LuxWindow(int width, int height, std::string title, SharedContextPtr context);
        LUX_PLATFORM_PUBLIC  LuxWindow(const InitParameter& parameter, SharedContextPtr context);
        
        /**
         * @brief init() won't be called automaticly
        */
        LUX_PLATFORM_PUBLIC          LuxWindow(int width, int height, std::string title);
        LUX_PLATFORM_PUBLIC explicit LuxWindow(const InitParameter& parameter);

        /**
         * @brief call after calling bindContext
        */
        [[nodiscard]] LUX_PLATFORM_PUBLIC bool init(std::shared_ptr<GraphicContext> context);

        [[nodiscard]] LUX_PLATFORM_PUBLIC bool isInitialized() const;

        LUX_PLATFORM_PUBLIC virtual ~LuxWindow();

        [[nodiscard]] LUX_PLATFORM_PUBLIC std::string title() const;

        [[nodiscard]] LUX_PLATFORM_PUBLIC WindowSize size() const;

        LUX_PLATFORM_PUBLIC bool shouldClose();

        LUX_PLATFORM_PUBLIC void swapBuffer();

        LUX_PLATFORM_PUBLIC void hideCursor(bool);

        [[nodiscard]] LUX_PLATFORM_PUBLIC KeyState queryKey(KeyEnum) const;

        LUX_PLATFORM_PUBLIC int  exec();

        /* Current version always return "glfw" */
        [[nodiscard]] LUX_PLATFORM_PUBLIC std::string  windowFrameworkName() const;

        LUX_PLATFORM_PUBLIC float lastFrameDelayTime() const;

        LUX_PLATFORM_PUBLIC WindowSize framebufferSize() const;

#ifdef __PLATFORM_WIN32__
        // Get windows 
        LUX_PLATFORM_PUBLIC void  win32Windows(void**);
#endif
        /*
         * @brief Enable or disable vsync
         * @param enable true to enable, false to disable
         */
        LUX_PLATFORM_PUBLIC static void     enableVsync(bool enable);
        LUX_PLATFORM_PUBLIC static void     pollEvents();
        LUX_PLATFORM_PUBLIC static void     waitEvents();
        LUX_PLATFORM_PUBLIC static double   timeAfterFirstInitialization();

        // Get glfw context
        LUX_PLATFORM_PUBLIC static GLFWwindow* currentContext();
        LUX_PLATFORM_PUBLIC static void makeContextCurrent(GLFWwindow*);

        using ProcPtr = void (*)();
        LUX_PLATFORM_PUBLIC static ProcPtr  getProcAddress(const char* name);

    protected:
        LUX_PLATFORM_PUBLIC virtual void    newFrame();

    private:

        LUX_PLATFORM_PUBLIC void subscribeKeyEvent();
        LUX_PLATFORM_PUBLIC void subscribeCursorPositionCallback();
        LUX_PLATFORM_PUBLIC void subscribeScrollCallback();
        LUX_PLATFORM_PUBLIC void subscribeMouseButtonCallback();
        LUX_PLATFORM_PUBLIC void subscribeWindowSizeChangeCallback();

        LUX_PLATFORM_PUBLIC bool visitContext(GLContext* gl_context, int operation) override;
        LUX_PLATFORM_PUBLIC bool visitContext(VulkanContext* vulkan_context, int operation)  override;

        float                           _delta_time{0};
        float                           _last_frame_time{0};

        GLFWwindow*                     _glfw_window;
        InitParameter                   _parameter;
        bool                            _init{ false };
        std::shared_ptr<GraphicContext> _context;
    };
} // namespace lux-engine::platform
