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
        bool presentation_pending{};
        std::size_t failed{}, settled{}, live_handles{}, pending_requests{};
        std::uint64_t serial_sum{};
        std::uint32_t asset_error{}, storage_error{};
    };
    struct LUX_EDITOR_SCENE_PUBLIC SceneWorkbenchMeasurement final
    {
        static WorkbenchMeasurement read(const SceneWorkbench &) noexcept;
    };
} // namespace lux::editor::workbench::detail
