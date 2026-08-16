#include <lux/engine/ui/ImGuiCommConfig.hpp>
#include <lux/engine/ui/ImGuiFeature.hpp>

#include <lux/engine/function/render/client/RenderRequest.hpp>
#include <lux/engine/function/render/client/RenderFrameSession.hpp>
#include <lux/engine/function/render/client/RenderProtocol.hpp>
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
    static lux::render::Expected<lux::render::FeatureHandle>
    imguiCreateFn(void* scene_ptr, const void* param, size_t param_size)
    {
        auto* sc = static_cast<lux::render::RenderScene*>(scene_ptr);

        const auto decoded = lux::render::decodeCommConfig<ImGuiCommConfig>(param, param_size);
        if (!decoded)
            return lux::cxx::unexpected(decoded.error());
        const ImGuiCommConfig& cfg = *decoded;

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

    // (ImGuiProxy::addUIView / removeUIView 已消亡:UI 目标统一走核心命令面
    //  RenderFrameSession::addView + createOffscreenRenderTarget(SAMPLED) + setLayer。)

} // namespace lux::ui
