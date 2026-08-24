#pragma once
/**
 * @file SkyboxComponent.hpp
 * @brief ECS-side description of "what the scene's skybox should be".
 *
 * Pair with an empty `EntityObject` (no transform needed — the skybox is
 * rendered as a fullscreen pass that samples the view direction).
 * `SkyboxSubsystem` bridges this component to the `Skybox` feature. Texture,
 * rotation and intensity are authored ECS facts; none of them live in a
 * scene-manifest bootstrap side channel.
 *
 * Only equirect (2D) textures are supported in this revision; cubemap
 * support comes later.
 */

// 组件是**数据**,桥是**行为**,方向必须是"桥看见组件"。
// 这里曾经 include <lux/engine/ecs/render/SceneRenderBinding.hpp> —— 一个
// 28-include 的枢纽头,被一个单字段 POD 整个拉进来,而头内一处都没用到它
// (下面注释里提到 ensureTexture,那是**桥**的事,发生在别的 TU)。
// 同行的 `using lux::ecs::SceneRenderBinding;` 更是个 no-op:本文件的
// 命名空间就是 lux::ecs。两行成对删除,谁都不受影响。
#include <lux/engine/resource/asset/Asset.hpp>          // asset_id_t
#include <lux/engine/meta/MetaAnnotations.hpp>

namespace lux::ecs
{
    struct LUX_COMPONENT() SkyboxComponent
    {
        /// UUID of the equirect TextureAsset to bind as the sky. The
        /// bridge resolves this via `SceneRenderBinding::ensureTexture`,
        /// inheriting the same cache + refcount model as material textures.
        LUX_MEMBER(display_name=Texture, asset_type=texture, tooltip=Equirectangular sky texture)
        lux::asset::asset_id_t equirect_texture_id;

        LUX_MEMBER(display_name=Rotation, tooltip=Rotation around the vertical axis in radians)
        float rotation_radians{0.0f};

        LUX_MEMBER(display_name=Intensity, min=0.0, tooltip=Linear sky radiance multiplier)
        float intensity{1.0f};
    };

} // namespace lux::ecs
