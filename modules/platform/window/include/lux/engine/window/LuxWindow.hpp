#pragma once
#include <memory>
#include <functional>
#include <string>
#include <lux/engine/window/LuxWindowDefination.hpp>
#include <lux/engine/platform/visibility.h>
#include "GraphicContext.hpp"
#include "Subwindow.hpp"

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
    class LuxWindowImpl;

    using KeyEventCallback          = std::function<void (LuxWindow&, KeyEnum, int, KeyState, ModifierKey)>;
    using CursorPoitionCallback     = std::function<void (LuxWindow&, double xpose, double ypose)>;
    using ScrollCallback            = std::function<void (LuxWindow&, double xoffset, double yoffset)>;
    using MouseButtonCallback       = std::function<void (LuxWindow&, MouseButton button, KeyState action, ModifierKey mods)>;
    using WindowSizeChangedCallbcak = std::function<void (LuxWindow&, int width, int height)>;

    using SharedContextPtr          = std::shared_ptr<GraphicContext>;

    template<class T, class... Args> SharedContextPtr createContext(Args&&... args)
    requires std::is_base_of_v<GraphicContext, T>
    {
        return std::make_shared<T>(std::forward<Args>(args)...);
    }

    class LuxWindow
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

        [[nodiscard]] LUX_PLATFORM_PUBLIC WindowSize windowSize() const;

        LUX_PLATFORM_PUBLIC bool shouldClose();

        LUX_PLATFORM_PUBLIC void swapBuffer();

        LUX_PLATFORM_PUBLIC void hideCursor(bool);

        [[nodiscard]] LUX_PLATFORM_PUBLIC KeyState queryKey(KeyEnum) const;

        LUX_PLATFORM_PUBLIC void subscribeKeyEvent(KeyEventCallback);
        LUX_PLATFORM_PUBLIC void subscribeCursorPositionCallback(CursorPoitionCallback);
        LUX_PLATFORM_PUBLIC void subscribeScrollCallback(ScrollCallback);
        LUX_PLATFORM_PUBLIC void subscribeMouseButtonCallback(MouseButtonCallback);
        LUX_PLATFORM_PUBLIC void subscribeWindowSizeChangeCallback(WindowSizeChangedCallbcak);

        LUX_PLATFORM_PUBLIC void addSubwindow(std::unique_ptr<Subwindow>);
        LUX_PLATFORM_PUBLIC int  exec();

        /* Current version always return "glfw" */
        [[nodiscard]] LUX_PLATFORM_PUBLIC std::string  windowFrameworkName() const;

#ifdef __PLATFORM_WIN32__
        // Get windows 
        LUX_PLATFORM_PUBLIC void            win32Windows(void**);
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

        using ProcPtr = void (*)();
        LUX_PLATFORM_PUBLIC static ProcPtr  getProcAddress(const char* name);

    protected:
        LUX_PLATFORM_PUBLIC virtual void paint();

    private:
        std::unique_ptr<LuxWindowImpl> _impl;
    };
} // namespace lux-engine::platform
