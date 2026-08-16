#include <lux/engine/ui/UIRenderFrameSession.hpp>
#include <lux/engine/function/render/client/protocol/RenderCommTypes.hpp>
#include <lux/engine/function/render/client/RenderProtocol.hpp>
#include <lux/engine/ui/ImGuiCommConfig.hpp>

namespace lux::ui
{
    UIRenderFrameSession::UIRenderFrameSession(
        std::shared_ptr<lux::render::RenderFrameChannel<>> channel,
        std::shared_ptr<lux::render::RenderChannelSync> sync,
        ImGuiOperationIds imgui_ops)
        : RenderFrameSession(std::move(channel), std::move(sync))
        , imgui_ops_(imgui_ops)
    {
    }

    UIRenderFrameSession::~UIRenderFrameSession() = default;

    void UIRenderFrameSession::submitImGuiDrawData(lux::render::RenderSceneId scene_id, ImDrawData *src)
    {
        if (!src || !src->Valid)
            return;

        auto& snapshot = snapshots_[frame_index_++ % kSnapshotCount];
        snapshot.snap(src);

        // Push draw data as borrowed attachment
        // (viewport events are bundled inside the snapshot)
        auto &b = builder();
        uint32_t att_idx = b.pushBorrowedObject(
            kImGuiDrawDataAttachment, &snapshot
        );

        lux::render::SubmitImGuiDrawDataPayload payload{};
        payload.scene_id = scene_id;
        payload.attachment_index = att_idx;
        b.push(lux::render::opcodes::CommandOp,
               imgui_ops_.submit_draw_data,
               payload);
    }
} // namespace lux::ui
