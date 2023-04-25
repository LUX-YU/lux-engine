#pragma once
#include <memory>
#include <functional>
#include <string>
#include <lux/window/LuxWindowDefination.hpp>
#include <lux/system/visibility_control.h>
#include "GraphicContext.hpp"

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
        LUX_EXPORT              LuxWindow(int width, int height, std::string title, SharedContextPtr context);
        LUX_EXPORT              LuxWindow(const InitParameter& parameter, SharedContextPtr context);
        /**
         * @brief init() won't be called automaticly
        */
        LUX_EXPORT              LuxWindow(int width, int height, std::string title);
        LUX_EXPORT     explicit LuxWindow(const InitParameter& parameter);

        /**
         * @brief call after calling bindContext
        */
        LUX_EXPORT bool         init(std::shared_ptr<GraphicContext> context);
        
        LUX_EXPORT bool         is_initialized();

        LUX_EXPORT virtual      ~LuxWindow();

        [[nodiscard]] LUX_EXPORT std::string  title() const;

        [[nodiscard]] LUX_EXPORT WindowSize   windowSize() const;

        LUX_EXPORT bool         shouldClose();

        LUX_EXPORT void         swapBuffer();

        LUX_EXPORT void         hideCursor(bool);

        [[nodiscard]] LUX_EXPORT KeyState     queryKey(KeyEnum) const;

        LUX_EXPORT void subscribeKeyEvent(KeyEventCallback);
        LUX_EXPORT void subscribeCursorPositionCallback(CursorPoitionCallback);
        LUX_EXPORT void subscribeScrollCallback(ScrollCallback);
        LUX_EXPORT void subscribeMouseButtonCallback(MouseButtonCallback);
        LUX_EXPORT void subscribeWindowSizeChangeCallback(WindowSizeChangedCallbcak);

        /* Current version always return "glfw" */
        [[nodiscard]] LUX_EXPORT std::string  windowFrameworkName() const;

        #ifdef __PLATFORM_WIN32__
        // Get windows 
        LUX_EXPORT void          win32Windows(void**);
        #endif
        
        LUX_EXPORT static void   enableVsync(bool enable);
        LUX_EXPORT static void   pollEvents();
        LUX_EXPORT static void   waitEvents();
        LUX_EXPORT static double timeAfterFirstInitialization();
        // Get glfw context
        LUX_EXPORT static GLFWwindow* currentContext();

        using ProcPtr = void (*)();
        LUX_EXPORT static ProcPtr getProcAddress(const char* name);
    private:
        std::unique_ptr<LuxWindowImpl> _impl;
    };
} // namespace lux-engine::platform
