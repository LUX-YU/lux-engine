#include <lux/engine/editor/ui/WindowOutput.hpp>
#include <lux/engine/window/LuxWindow.hpp>

namespace lux::editor::ui
{
    EditorResult<lux::scene::NativeSurfaceOutput> windowOutput(window::LuxWindow& window) noexcept
    {
#if defined(_WIN32)
        const auto handle = window.nativeHandle();
        if (handle)
            return lux::scene::NativeSurfaceOutput{reinterpret_cast<std::uintptr_t>(handle)};
#endif
        return lux::cxx::unexpected(EditorFailure{
            EEditorError::FRONTEND_FAILURE,
            "editor.surface",
            0,
            "Native presentation is unavailable for this window"
        });
    }
}
