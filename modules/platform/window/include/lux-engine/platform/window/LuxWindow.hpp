#pragma once
#include <memory>
#include <functional>
#include <lux-engine/platform/window/LuxWindowDefination.hpp>
#include <lux-engine/platform/system/visibility_control.h>

#ifdef __PLATFORM_WIN32__
#   include <windows.h>
#endif

struct GLFWwindow;

namespace lux::engine::platform
{
    struct InitParameter
    {
        int         width;
        int         height;
        std::string title;
        GraphicAPI  graphic_api;
    };

    // not copyable
    // movable
    class LuxWindow
    {
    public:
        LUX_EXPORT              LuxWindow(int width, int height, std::string title, GraphicAPI api = GraphicAPI::OPENGL);

        LUX_EXPORT              LuxWindow(const InitParameter& parameter);

        LUX_EXPORT              ~LuxWindow();

        LUX_EXPORT std::string  title() const;

        LUX_EXPORT WindowSize   windowSize() const;

        LUX_EXPORT bool         shouldClose();

        LUX_EXPORT void         swapBuffer();

        LUX_EXPORT void         hideCursor(bool);

        LUX_EXPORT KeyState     queryKey(KeyEnum) const;

        using KeyEventCallback      = std::function<void (LuxWindow&, KeyEnum, int, KeyState, ModifierKey)>;
        using CursorPoitionCallback = std::function<void (LuxWindow&, double xpose, double ypose)>;
        using ScrollCallback        = std::function<void (LuxWindow&, double xoffset, double yoffset)>;
        using MouseButtonCallback   = std::function<void (LuxWindow&, MouseButton button, KeyState action, ModifierKey mods)>;

        LUX_EXPORT void         subscribeKeyEvent(KeyEventCallback);
        LUX_EXPORT void         subscribeCursorPositionCallback(CursorPoitionCallback);
        LUX_EXPORT void         subscribeScrollCallback(ScrollCallback);
        LUX_EXPORT void         subscribeMouseButtonCallback(MouseButtonCallback);

        /* current version always return GraphicAPI::OPENGL */
        LUX_EXPORT GraphicAPI   graphicAPIType() const;
        /* current version always return "glfw" */
        LUX_EXPORT std::string  windowFrameworkName() const;

        #ifdef __PLATFORM_WIN32__
        // get windows 
        LUX_EXPORT HWND         win32Windows();
        #endif
        
        LUX_EXPORT static void  enableVsync(bool enable);
        LUX_EXPORT static void  pollEvents();
        LUX_EXPORT static void  waitEvents();
        LUX_EXPORT static double timeAfterFirstInitialization();
        // get glfw context
        LUX_EXPORT static GLFWwindow* currentContext();

        using ProcPtr = void (*)();
        LUX_EXPORT static ProcPtr getProcAddress(const char* name);
    private:

        class Impl;
        std::unique_ptr<Impl> _impl;
    };

    
} // namespace lux-engine::platform
