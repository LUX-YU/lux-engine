#pragma once
/**
 * @file SkyboxComponent.hpp
 * @brief ECS-side description of "what the scene's skybox should be".
 *
 * Pair with an empty `EntityObject` (no transform needed — the skybox is
 * rendered as a fullscreen pass that samples the view direction). The PARAM
 * render bridge (`EcsRenderTraits<SkyboxComponent>`) bridges this component to
 * the `Skybox` feature by calling `SkyboxProxy::setEquirect(...)` whenever
 * the texture asset id changes.
 *
 * Only equirect (2D) textures are supported in this revision; cubemap
 * support comes later.
 */

#include <lux/engine/asset/Asset.hpp>          // asset_id_t
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/render_bridge/RenderableBridgeContext.hpp>

namespace lux::pack
{
    using lux::render_bridge::RenderableBridgeContext;

    struct LUX_COMPONENT() SkyboxComponent
    {
        /// UUID of the equirect TextureAsset to bind as the sky. The
        /// bridge resolves this via `RenderableBridgeContext::ensureTexture`,
        /// inheriting the same cache + refcount model as material textures.
        lux::asset::asset_id_t equirect_texture_id;
    };

} // namespace lux::pack
