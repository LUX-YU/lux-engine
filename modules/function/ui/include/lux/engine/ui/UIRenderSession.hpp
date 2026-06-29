#pragma once
// ============================================================================
//  UIRenderSession — RenderSession subclass with ImGui draw-data submission
// ============================================================================

#include <lux/engine/render/comm/client/RenderSession.hpp>
#include <lux/engine/ui/ImGuiCommConfig.hpp>
#include <lux/engine/ui/ImGuiDrawDataSnapshot.hpp>
#include <lux/engine/function/visibility_ui.h>

#include <cstdint>

struct ImDrawData;

namespace lux::ui
{
    class LUX_FUNCTION_UI_PUBLIC UIRenderSession : public lux::render::RenderSession
    {
    public:
        explicit UIRenderSession(
            std::shared_ptr<lux::render::RenderProgramChannel<>> channel,
            std::shared_ptr<lux::render::RenderChannelSync> sync,
            ImGuiOperationIds imgui_ops);

        /// Construct without pre-known ImGui ops — discovers them via queryTypeId.
        /// Blocks until the server has processed the queries.
        explicit UIRenderSession(
            std::shared_ptr<lux::render::RenderProgramChannel<>> channel,
            std::shared_ptr<lux::render::RenderChannelSync> sync
        );

        ~UIRenderSession();

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
