#include <lux/engine/ui/UIRenderSession.hpp>
#include <lux/engine/render/comm/RenderCommTypes.hpp>
#include <lux/engine/render/comm/RenderProtocol.hpp>
#include <lux/engine/ui/ImGuiCommConfig.hpp>

namespace lux::ui
{
    UIRenderSession::UIRenderSession(
        std::shared_ptr<lux::render::RenderProgramChannel<>> channel,
        std::shared_ptr<lux::render::RenderChannelSync> sync,
        ImGuiOperationIds imgui_ops)
        : RenderSession(std::move(channel), std::move(sync))
        , imgui_ops_(imgui_ops)
    {
    }

    UIRenderSession::UIRenderSession(
        std::shared_ptr<lux::render::RenderProgramChannel<>> channel,
        std::shared_ptr<lux::render::RenderChannelSync> sync)
        : RenderSession(std::move(channel), std::move(sync))
    {
        beginFrame({});
        auto q1 = queryTypeId(imgui_ops::kSubmitDrawData);
        auto q2 = queryTypeId(imgui_ops::kAddUIView);
        auto q3 = queryTypeId(imgui_ops::kRemoveUIView);
        submitFrame(true);
        while (!q1.isReady() || !q2.isReady() || !q3.isReady())
            waitAndPumpReplies();

        imgui_ops_.submit_draw_data = q1.result().type_id;
        imgui_ops_.add_ui_view      = q2.result().type_id;
        imgui_ops_.remove_ui_view   = q3.result().type_id;

        beginFrame({});
    }

    UIRenderSession::~UIRenderSession() = default;

    void UIRenderSession::submitImGuiDrawData(lux::render::RenderSceneId scene_id, ImDrawData *src)
    {
        if (!src || !src->Valid)
            return;

        auto& snapshot = snapshots_[frame_index_++ % kSnapshotCount];
        snapshot.snap(src);

        // Push draw data as borrowed attachment
        // (viewport events are bundled inside the snapshot)
        auto &b = builder();
        uint32_t att_idx = b.pushBorrowedObject(
            lux::render::attachment_types::ImGuiDrawData, &snapshot
        );

        lux::render::SubmitImGuiDrawDataPayload payload{};
        payload.scene_id = scene_id;
        payload.attachment_index = att_idx;
        b.push(lux::render::opcodes::CommandOp,
               imgui_ops_.submit_draw_data,
               payload);
    }
} // namespace lux::ui
