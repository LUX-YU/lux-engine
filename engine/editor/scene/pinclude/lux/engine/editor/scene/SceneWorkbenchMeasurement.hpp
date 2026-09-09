#pragma once
#include <lux/engine/editor/scene/SceneWorkbench.hpp>

namespace lux::editor::workbench::detail
{
    // Read-only observation for the non-installed baseline cost consumer. No model or writer escapes.
    struct WorkbenchMeasurement final
    {
        lux::render::RenderTargetId target;
        std::shared_ptr<const void> target_reference;
        std::uint32_t width{}, height{};
        std::size_t resources{}, ready{};
        bool resizing{};
    };
    struct LUX_EDITOR_SCENE_PUBLIC SceneWorkbenchMeasurement final
    {
        static WorkbenchMeasurement read(const SceneWorkbench &) noexcept;
    };
} // namespace lux::editor::workbench::detail
