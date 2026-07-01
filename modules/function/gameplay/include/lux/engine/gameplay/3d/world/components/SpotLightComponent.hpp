#pragma once
/**
 * @file SpotLightComponent.hpp
 * @brief ECS analogue of `lux::render::SpotLightDesc`.
 *
 * One entity carrying this component yields one cone-shaped spot light.
 * The POOL render bridge (`EcsRenderTraits<SpotLightComponent>`) bridges it to
 * `LightProxy::createLight` / `updateLight` / `destroyLight`.
 *
 * Position comes from the entity's WorldTransformComponent (like
 * PointLightComponent — a light + visible bulb can share one entity). The cone
 * `direction` is stored on the component (a world-space unit vector the cone
 * points toward), kept independent of the transform's rotation so the demo can
 * aim each spot explicitly.
 */

#include <Eigen/Core>
#include <cstdint>

#include <lux/engine/meta/MetaAnnotations.hpp>

namespace lux::gameplay::d3
{
    struct LUX_COMPONENT() SpotLightComponent
    {
        LUX_MEMBER(display_name=Direction, tooltip=Unit world-space direction the cone points toward)
        Eigen::Vector3f direction{0.f, -1.f, 0.f};

        LUX_MEMBER(display_name=Color, color=true, tooltip=Linear RGB tint of the light)
        Eigen::Vector3f color{1.f, 1.f, 1.f};

        LUX_MEMBER(display_name=Intensity, min=0.0)
        float intensity = 6.f;

        /// Reach: range-windowed falloff (see PointLightComponent). Unbounded in
        /// the Inspector.
        LUX_MEMBER(display_name=Range, min=0.1, tooltip=World-space reach; light smoothly fades to zero here)
        float range = 150.f;

        /// Optional extra polynomial falloff (default off → range controls reach).
        LUX_MEMBER(display_name=Attenuation Constant, min=0.0)
        float attenuation_constant = 1.f;

        LUX_MEMBER(display_name=Attenuation Linear, min=0.0)
        float attenuation_linear = 0.f;

        LUX_MEMBER(display_name=Attenuation Quadratic, min=0.0)
        float attenuation_quadratic = 0.f;

        LUX_MEMBER(display_name=Inner Cone Angle, min=0.0, max=1.5708, tooltip=Radians; full intensity inside this half-angle)
        float inner_cone_angle = 0.40f;   // ~23°

        LUX_MEMBER(display_name=Outer Cone Angle, min=0.0, max=1.5708, tooltip=Radians; falls to zero by this half-angle)
        float outer_cone_angle = 0.61f;   // ~35°

        /// Whether this light renders into the shadow atlas. A spot shadow is a
        /// single perspective slice and competes for a Top-K shadow-atlas slot.
        /// The bridge folds it into `SpotLightDesc::flags` bit 0
        /// (`LIGHT_FLAG_CAST_SHADOW`).
        LUX_MEMBER(display_name=Cast Shadow, tooltip=Render this light into the shadow map)
        bool cast_shadow = true;

        LUX_MEMBER(display_name=Shadow Map Size, min=128, max=8192)
        std::uint32_t shadow_map_size = 1024;

        LUX_MEMBER(display_name=Shadow Bias, min=0.0, max=0.1)
        float shadow_bias = 0.005f;

        LUX_MEMBER(display_name=Shadow Normal Bias, min=0.0, max=0.5)
        float shadow_normal_bias = 0.01f;
    };

} // namespace lux::gameplay::d3
