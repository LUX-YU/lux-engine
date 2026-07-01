#pragma once
/**
 * @file DirectionalLightComponent.hpp
 * @brief ECS analogue of `lux::render::DirectionalLightDesc`.
 *
 * One entity carrying this component yields one directional light in the
 * scene. The POOL render bridge (`EcsRenderTraits<DirectionalLightComponent>`)
 * translates between the component and `LightProxy::createLight` / `updateLight`.
 *
 * Fields are kept symmetric with `DirectionalLightDesc` so the bridge can
 * just copy. Direction is the **unit world-space vector the light shines
 * towards** (i.e. opposite of "light position from sun"); intentionally
 * NOT derived from a transform — this keeps the light placeable
 * independently of any entity hierarchy.
 */

#include <Eigen/Core>
#include <array>
#include <cstdint>

#include <lux/engine/meta/MetaAnnotations.hpp>

namespace lux::gameplay::d3
{
    struct LUX_COMPONENT() DirectionalLightComponent
    {
        LUX_MEMBER(display_name=Direction, tooltip=Unit world-space direction the light shines toward)
        Eigen::Vector3f direction{0.f, -1.f, 0.f};

        LUX_MEMBER(display_name=Color, color=true, tooltip=Linear RGB tint of the light)
        Eigen::Vector3f color{1.f, 1.f, 1.f};

        LUX_MEMBER(display_name=Intensity, min=0.0, max=50.0)
        float intensity = 1.f;

        /// Whether the sun renders cascaded shadow maps. The directional light
        /// is the cheapest, highest-impact caster, so this defaults on. The
        /// bridge folds it into `DirectionalLightDesc::flags` bit 0
        /// (`LIGHT_FLAG_CAST_SHADOW`) in LightRenderTraits.
        LUX_MEMBER(display_name=Cast Shadow, tooltip=Render cascaded shadow maps for this light)
        bool cast_shadow = true;

        LUX_MEMBER(display_name=Shadow Map Size, min=128, max=8192, tooltip=Resolution of each shadow cascade map in texels)
        std::uint32_t shadow_map_size = 1024;

        // Constant depth bias applied at shadow-map rasterisation
        // (vkCmdSetDepthBias constant factor, scaled by the depth-buffer
        // unit). Tiny floating-point tie-breaker; the geometric component
        // of self-shadow prevention is handled by slope-scale rasterizer
        // bias (≈shadow_bias × 2) + front-face culling on the shadow
        // pass + receiver-plane depth bias inside PCF. No manual
        // `shadow_normal_bias` parameter — the receiver-plane gradient is
        // computed per-fragment from dFdx/dFdy and applied per PCF tap,
        // which is mathematically exact rather than a tuned offset.
        LUX_MEMBER(display_name=Shadow Bias, min=0.0, max=0.1)
        float shadow_bias = 0.005f;

        LUX_MEMBER(display_name=Cascade Count, min=1, max=8, tooltip=Number of cascaded shadow map slices to use)
        std::uint32_t cascade_count = 4;

        /// Cascade split distances (camera-space far Z of each slice).
        /// Custom widget needed for full editing — first N entries based on
        /// `cascade_count`. Not annotated for primitive Inspector use yet.
        std::array<float, 8> cascade_splits{0.1f, 0.25f, 0.5f, 1.0f, 0.f, 0.f, 0.f, 0.f};
    };

} // namespace lux::gameplay::d3
