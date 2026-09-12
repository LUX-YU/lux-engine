#pragma once
#include <lux/engine/editor/scene/SceneWorkbench.hpp>
#include <filesystem>

namespace lux::editor::workbench::detail
{
    // Build-test-only diagnostic owner. This header is never installed.
    struct LUX_EDITOR_SCENE_PUBLIC SceneWorkbenchDiagnostics final
    {
        static void enable(SceneWorkbench& workbench, const std::filesystem::path& directory);
        static bool passed(const SceneWorkbench& workbench) noexcept;
    };
}
