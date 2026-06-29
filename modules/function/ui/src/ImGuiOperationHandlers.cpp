#include <lux/engine/ui/ImGuiCommConfig.hpp>
#include <lux/engine/ui/ImGuiFeature.hpp>

#include <lux/engine/render/comm/client/RenderRequest.hpp>
#include <lux/engine/render/comm/client/RenderSession.hpp>
#include <lux/engine/render/comm/RenderProtocol.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>

#include <cstring>

namespace lux::ui
{
    // Defined in UIRenderServer.cpp — registers SubmitImGuiDrawData, AddUIView, RemoveUIView
    uint32_t imguiRegisterOpsFn(void* dispatcher, lux::render::TypeId* out_ops, uint32_t max_ops);
    void     imguiUnregisterOpsFn(void* dispatcher, const lux::render::TypeId* ops, uint32_t op_count);

    namespace
    {
        VkFormat textureFormatToVk(lux::render::ETextureFormatHint f) noexcept
        {
            switch (f)
            {
            case lux::render::ETextureFormatHint::RGBA8:   return VK_FORMAT_R8G8B8A8_UNORM;
            case lux::render::ETextureFormatHint::SRGBA8:  return VK_FORMAT_R8G8B8A8_SRGB;
            case lux::render::ETextureFormatHint::BGRA8:   return VK_FORMAT_B8G8R8A8_UNORM;
            case lux::render::ETextureFormatHint::SBGRA8:  return VK_FORMAT_B8G8R8A8_SRGB;
            case lux::render::ETextureFormatHint::RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
            case lux::render::ETextureFormatHint::RGBA32F: return VK_FORMAT_R32G32B32A32_SFLOAT;
            default:                                   return VK_FORMAT_UNDEFINED;
            }
        }
    }

    // ── FeatureFactory callbacks ─────────────────────────────────────────
    static lux::render::FeatureHandle imguiCreateFn(void* scene_ptr, const void* param, size_t param_size)
    {
        auto* sc = static_cast<lux::render::RenderScene*>(scene_ptr);

        ImGuiCommConfig cfg{};
        if (param && param_size >= sizeof(ImGuiCommConfig))
            cfg = *static_cast<const ImGuiCommConfig*>(param);

        return sc->addFeature<ImGuiFeature>(
            textureFormatToVk(cfg.color_format),
            cfg.font_pixels, cfg.font_width, cfg.font_height
        );
    }

    // ── Exported factory instance ────────────────────────────────────────
    const lux::render::FeatureFactory kImGuiFeatureFactory{
        &imguiCreateFn,
        &imguiRegisterOpsFn,
        &imguiUnregisterOpsFn,
    };

    // =====================================================================
    //  ImGuiProxy — client-side proxy
    // =====================================================================

    using namespace lux::render;

    void ImGuiProxy::submitDrawData(uint32_t attachment_index)
    {
        SubmitImGuiDrawDataPayload payload{};
        payload.attachment_index = attachment_index;
        session_->builder().push(opcodes::CommandOp, ops_.submit_draw_data, payload);
    }

    RenderRequest<ViewCreatedReply> ImGuiProxy::addUIView(
        RenderSceneId scene_id,
        common::Size2D extent,
        const char* name,
        const float* initial_view_matrix,
        const float* initial_proj_matrix,
        const float* initial_camera_position)
    {
        auto [req, cb] = RenderRequestFactory<ViewCreatedReply>::make();

        AddViewPayload avp{};
        avp.scene_id     = scene_id;
        avp.extent       = extent;
        if (name)
            std::strncpy(avp.name, name, sizeof(avp.name) - 1);
        // (initial camera removed from AddView — View 去 3D 化. The camera comes via the
        //  StandardViewCamera op now; these params are ignored. Follow-up: drop them from
        //  addUIView + its callers, a UI-module tidy-up.)
        (void)initial_view_matrix;
        (void)initial_proj_matrix;
        (void)initial_camera_position;

        session_->builder().pushWithReply(opcodes::CommandOp, ops_.add_ui_view, avp, std::move(cb));
        return req;
    }

    void ImGuiProxy::removeUIView(RenderSceneId scene_id, ViewHandle view)
    {
        RemoveViewPayload rvp{};
        rvp.scene_id = scene_id;
        rvp.view     = view;
        session_->builder().push(opcodes::CommandOp, ops_.remove_ui_view, rvp);
    }

} // namespace lux::ui
