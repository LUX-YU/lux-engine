#include <lux/engine/editor/desktop/WindowOutput.hpp>
#include <lux/engine/window/LuxWindow.hpp>

namespace lux::editor::desktop
{
    DesktopResult<lux::scene::NativeSurfaceOutput> windowOutput(window::LuxWindow& window) noexcept
    {
#if defined(_WIN32)
        const auto handle = window.nativeHandle();
        if (handle)
            return lux::scene::NativeSurfaceOutput{reinterpret_cast<std::uintptr_t>(handle)};
#endif
        return lux::cxx::unexpected(DesktopFailure{"desktop.surface", render::ERendererError::INVALID_ARGUMENT});
    }
}
