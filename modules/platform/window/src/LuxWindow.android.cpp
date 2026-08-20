/**
 * @file LuxWindow.android.cpp
 * @brief Android window backend — COMPILE-LEVEL SKELETON.
 *
 * Purpose today: let the Android cross-configure/build proceed past the
 * window module so the remaining dependency walls (vcpkg triplets, host-tool
 * call sites) surface in order. Every method is a well-defined no-op; no
 * windowing library is touched (the GLFWwindow* member in the header is only
 * a forward-declared pointer and stays null).
 *
 * The REAL backend (ANativeWindow surface + vkCreateAndroidSurfaceKHR +
 * GameActivity/ALooper pump + APP_CMD_INIT/TERM_WINDOW lifecycle) lands
 * after the LuxWindow.hpp header-neutralization surgery — see the 2026-07-20
 * progress notes in .internal/lux-engine-mobile-adaptation-investigation.md
 * (§3.2 design, "统一头收敛是 3-3 的前置").
 */

#include <lux/engine/window/LuxWindow.hpp>

#include <android/native_window.h>
// VK_USE_PLATFORM_ANDROID_KHR must precede vulkan.h for the Android surface
// declarations (VkAndroidSurfaceCreateInfoKHR, PFN_vkCreateAndroidSurfaceKHR).
#ifndef VK_USE_PLATFORM_ANDROID_KHR
#   define VK_USE_PLATFORM_ANDROID_KHR 1
#endif
#include <vulkan/vulkan.h>

namespace lux::window
{
    LuxWindow::LuxWindow(int width, int height, std::string title)
        : _parameter{width, height, std::move(title)}
    {
    }

    LuxWindow::LuxWindow(const InitParameter& parameter)
        : _parameter{parameter}
    {
    }

    LuxWindow::~LuxWindow() = default;

    bool LuxWindow::init()
    {
        // No surface yet on Android at construction time: the window becomes
        // real only at APP_CMD_INIT_WINDOW. The skeleton reports success so
        // bring-up code paths can be exercised in unit contexts.
        _init = true;
        return true;
    }

    bool LuxWindow::isInitialized() const { return _init; }

    const char* LuxWindow::title() const { return _parameter.title.c_str(); }

    void LuxWindow::size(
        std::uint32_t& width,
        std::uint32_t& height
    ) const
    {
        width = static_cast<std::uint32_t>(_parameter.width);
        height = static_cast<std::uint32_t>(_parameter.height);
    }

    bool LuxWindow::shouldClose() { return false; }

    void LuxWindow::hideCursor(bool) {}

    bool LuxWindow::setRawMouseMotion(bool) { return false; }

    void LuxWindow::getCursorPos(double* x, double* y) const
    {
        if (x)
        {
            *x = 0.0;
        }
        if (y)
        {
            *y = 0.0;
        }
    }

    void LuxWindow::setCursorPos(double, double) {}

    KeyState LuxWindow::queryKey(KeyEnum) const { return KeyState::RELEASE; }

    InputSnapshot LuxWindow::captureInputSnapshot()
    {
        InputSnapshot snapshot{};
        snapshot.window_width      = static_cast<uint32_t>(_parameter.width);
        snapshot.window_height     = static_cast<uint32_t>(_parameter.height);
        return snapshot;
    }

    int LuxWindow::exec() { return 0; }

    void LuxWindow::setExitBehavior(EExitBehavior behavior) { _exit_behavior = behavior; }

    void LuxWindow::exit() {}

    void LuxWindow::hide(bool) {}

    std::string LuxWindow::windowFrameworkName() const { return "android-stub"; }

    float LuxWindow::lastFrameDelayTime() const { return _delta_time; }

    void LuxWindow::framebufferSize(
        std::uint32_t& width,
        std::uint32_t& height
    ) const
    {
        size(width, height);
    }

    bool LuxWindow::createVulkanSurface(VkInstance,
                                        const VkAllocationCallbacks*,
                                        VkSurfaceKHR* out_surface)
    {
        // Intentionally always false on Android, and NOT a gap to fill later.
        //
        // This overload's contract is "the window object owns a surface it can
        // hand out", which is a desktop notion. Android's window arrives from
        // the OS at APP_CMD_INIT_WINDOW and is revoked at TERM_WINDOW, several
        // times per session — its lifetime matches a surface, not a window
        // object, so a LuxWindow cannot honestly own one.
        //
        // The real path is RenderSurface::initFromNative(ANativeWindow handle),
        // reached through the CreateSurfaceTarget command. Callers on Android
        // must use it; this returning false is what keeps them from silently
        // going through the desktop-shaped door.
        if (out_surface)
        {
            *out_surface = VK_NULL_HANDLE;
        }
        return false;
    }

    std::span<const char* const> LuxWindow::requiredVulkanInstanceExtensions()
    {
        static constexpr const char* kExtensions[] = {
            "VK_KHR_surface",
            "VK_KHR_android_surface",
        };
        return std::span<const char* const>{kExtensions};
    }

    void LuxWindow::pollEvents() {}

    void LuxWindow::waitEvents() {}

    double LuxWindow::timeAfterFirstInitialization() { return 0.0; }

    GLFWwindow* LuxWindow::handle() { return nullptr; }

    GLFWwindow* LuxWindow::currentContext() { return nullptr; }

    void LuxWindow::makeContextCurrent(GLFWwindow*) {}

    LuxWindow::ProcPtr LuxWindow::getProcAddress(const char*) { return nullptr; }

    bool LuxWindow::vulkanSupported() { return true; }

    void LuxWindow::newFrame() {}

} // namespace lux::window
