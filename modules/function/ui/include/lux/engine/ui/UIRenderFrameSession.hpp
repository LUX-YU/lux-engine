#pragma once
// ============================================================================
//  UIRenderFrameSession — RenderFrameSession subclass with ImGui draw-data submission
// ============================================================================

#include <lux/engine/function/render/client/RenderFrameSession.hpp>
#include <lux/engine/ui/ImGuiCommConfig.hpp>
#include <lux/engine/ui/ImGuiDrawDataSnapshot.hpp>
#include <lux/engine/function/visibility_ui.h>

#include <cstdint>

struct ImDrawData;

namespace lux::ui
{
    class LUX_FUNCTION_UI_PUBLIC UIRenderFrameSession : public lux::render::RenderFrameSession
    {
    public:
        explicit UIRenderFrameSession(
            std::shared_ptr<lux::render::RenderFrameChannel<>> channel,
            std::shared_ptr<lux::render::RenderChannelSync> sync,
            ImGuiOperationIds imgui_ops);

        ~UIRenderFrameSession();

        /// Snapshot the current ImDrawData and submit it as a borrowed
        /// attachment to the render thread for the given ImGui scene.
        /// Viewport lifecycle events are bundled automatically inside the snapshot.
        void submitImGuiDrawData(lux::render::RenderSceneId scene_id, ImDrawData *src);

        /// Access the operation IDs for ImGui ops.
        [[nodiscard]] const ImGuiOperationIds& imguiOps() const noexcept { return imgui_ops_; }

    private:
        static constexpr std::uint32_t kSnapshotCount = 4;
        ImDrawDataSnapshot snapshots_[kSnapshotCount];
        std::uint32_t      frame_index_{0};
        ImGuiOperationIds  imgui_ops_;
    };
} // namespace lux::ui
