#pragma once
/**
 * @file SkyboxRenderTraits.hpp
 * @brief EcsRenderTraits<SkyboxComponent> — PARAM. Resolves the equirect texture
 *        (async, via the shared texture cache) and pushes SkyboxProxy::setEquirect
 *        when the resolved handle changes. extract() returns nullopt until the
 *        texture is ready (skip this frame, retry). Replaces the hand-written Skybox adapter.
 */

#include <optional>

#include <lux/engine/render/core/FeatureHandle.hpp>                        // FeatureHandle
#include <lux/engine/render/core/RenderResourceHandle.hpp>                 // RTextureHandle
#include <lux/engine/render/core/RenderSceneId.hpp>                        // RenderSceneId
#include <lux/engine/render/comm/client/RenderSession.hpp>                 // RenderSession
#include <lux/engine/render/renderer/features/sky_box/SkyboxOperation.hpp> // SkyboxProxy / SkyboxOperationIds

#include "lux/pack/d3/world/components/SkyboxComponent.hpp"
#include "lux/engine/render_bridge/RenderableBridgeContext.hpp"
#include "lux/engine/render_bridge/EcsRenderTraits.hpp"

namespace lux::render_bridge
{
    using lux::pack::SkyboxComponent;   // (a lux::pack component)

    template <>
    struct EcsRenderTraits<SkyboxComponent>
    {
        static constexpr ERenderableKind kind    = ERenderableKind::PARAM;
        static constexpr const char*     feature = "Skybox";
        using Ops = lux::render::SkyboxOperationIds;

        /// Dirty key = the resolved equirect texture handle (+ scene/feature for push).
        struct Payload
        {
            lux::render::RenderSceneId  scene{};
            lux::render::FeatureHandle  feature{};
            lux::render::RTextureHandle tex{};
        };

        static std::optional<Payload> extract(const SkyboxComponent& sc, RenderableBridgeContext& ctx,
                                              lux::render::FeatureHandle feat)
        {
            if (sc.equirect_texture_id.is_nil()) return std::nullopt;          // nothing set
            const auto tex = ctx.ensureTexture(sc.equirect_texture_id);
            if (tex.is_null()) return std::nullopt;                            // upload pending → retry next frame
            return Payload{ ctx.scene(), feat, tex };
        }

        static void push(lux::render::RenderSession& s, const Ops& ops, const Payload& p)
        {
            lux::render::SkyboxProxy(s, ops).setEquirect(p.scene, p.feature, p.tex);
        }

        /// Teardown reset (G-06): unbind the skybox on the server so a REUSED scene does
        /// not keep sampling a texture the bridge's teardown already released (P1-1). A
        /// null-texture setEquirect drops the feature to ActiveMode::NONE. PARAM has no
        /// async create, so this is synchronous; ParamBridge::beginShutdown calls it.
        static void clear(RenderableBridgeContext& ctx)
        {
            const auto feat = ctx.features().handle(feature);
            if (!feat.valid()) return;   // feature absent → nothing bound → nothing to clear
            const auto ops = ctx.features().ops<Ops>(feature);
            lux::render::SkyboxProxy(ctx.session(), ops)
                .setEquirect(ctx.scene(), feat, lux::render::RTextureHandle{});
        }
    };

} // namespace lux::render_bridge
