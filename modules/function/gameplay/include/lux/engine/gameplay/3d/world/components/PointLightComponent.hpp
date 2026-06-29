#pragma once
/**
 * @file PointLightComponent.hpp
 * @brief ECS analogue of `lux::render::PointLightDesc`.
 *
 * One entity carrying this component yields one omni-directional point light.
 * The POOL render bridge (`EcsRenderTraits<PointLightComponent>`) bridges it to
 * `LightProxy::createLight` / `updateLight` / `destroyLight`.
 *
 * Unlike `DirectionalLightComponent` (which stores its direction), a point
 * light's **world position is taken from the entity's WorldTransformComponent**
 * — so a light and a co-located visible "bulb" mesh can live on the SAME
 * entity (move the transform → both follow). The bridge requires the entity
 * to carry a WorldTransformComponent (the editor ensures one for every
 * transform-bearing entity after a scene load).
 */

#include <Eigen/Core>
#include <cstdint>

#include <lux/engine/meta/MetaAnnotations.hpp>

namespace lux::gameplay::d3
{
    struct LUX_COMPONENT() PointLightComponent
    {
        LUX_MEMBER(display_name=Color, color=true, tooltip=Linear RGB tint of the light)
        Eigen::Vector3f color{1.f, 1.f, 1.f};

        LUX_MEMBER(display_name=Intensity, min=0.0)
        float intensity = 4.f;

        /// Reach: the shader uses a range-windowed falloff ((1-(d/range)^4)^2),
        /// so `range` IS where the light fades to zero. Unbounded (continuous,
        /// no upper cap) in the Inspector — set it as large as you like.
        LUX_MEMBER(display_name=Range, min=0.1, tooltip=World-space reach; light smoothly fades to zero here)
        float range = 120.f;

        /// OPTIONAL extra polynomial falloff on top of the range window
        /// (default off: constant=1, linear=0, quadratic=0 → range alone controls
        /// the reach). Raise linear/quadratic for a faster near-light dropoff.
        LUX_MEMBER(display_name=Attenuation Constant, min=0.0)
        float attenuation_constant = 1.f;

        LUX_MEMBER(display_name=Attenuation Linear, min=0.0)
        float attenuation_linear = 0.f;

        LUX_MEMBER(display_name=Attenuation Quadratic, min=0.0)
        float attenuation_quadratic = 0.f;

        /// Bitfield mirroring `lux::render::LightDescriptorFlags`.
        /// Bit 0 = `LIGHT_FLAG_CAST_SHADOW` (point shadow = a cubemap, costly —
        /// leave 0 for most lights). Not Inspector-edited as a primitive.
        std::uint32_t flags = 0;

        LUX_MEMBER(display_name=Shadow Map Size, min=128, max=8192)
        std::uint32_t shadow_map_size = 1024;

        LUX_MEMBER(display_name=Shadow Bias, min=0.0, max=0.1)
        float shadow_bias = 0.005f;

        LUX_MEMBER(display_name=Shadow Normal Bias, min=0.0, max=0.5)
        float shadow_normal_bias = 0.01f;
    };

} // namespace lux::gameplay::d3
