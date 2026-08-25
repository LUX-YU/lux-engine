#pragma once
// ============================================================================
//  LightDescriptor.hpp — Render-domain light description types
//
//  These are the "boundary DTO" types used to decouple the resources layer from
//  ECS component types (light facts and resolved transforms). Conversion
//  from ECS components to these descriptors occurs via lightDescriptorFromComponent().
// ============================================================================
#include <Eigen/Dense>
#include <lux/engine/function/render/client/core/RenderSpatialTypes.hpp>
#include <cstdint>
#include <variant>
#include <array>
// Zero-overhead annotation macros (expand to nothing unless __LUX_PARSE_TIME__ is defined).
#include <lux/engine/meta/MetaAnnotations.hpp>

namespace lux::render
{
    // ===== Common light flags =====
    enum LightDescriptorFlags : uint32_t {
        LIGHT_FLAG_CAST_SHADOW = 1u << 0,
    };

    // ===== Directional-cascade counts =====
    //
    // Two different numbers, previously both spelled as bare literals in six
    // places, and they are NOT the same number:
    //
    //   · kMaxShadowCascades  — how many cascades the renderer will actually
    //     build. ShadowMapFeature clamps cascade_count to this.
    //   · kShadowCascadeSlots — how many slots the split array carries, in this
    //     descriptor and in the GPU mirror. Larger than the first, so raising
    //     the cascade limit does not immediately change the GPU struct layout.
    //
    // Keeping both named makes the gap deliberate rather than looking like one
    // of the two is a typo. Reading past kMaxShadowCascades yields the trailing
    // zeros the default initialiser puts there.
    inline constexpr uint32_t kMaxShadowCascades  = 4;
    inline constexpr uint32_t kShadowCascadeSlots = 8;
    static_assert(kMaxShadowCascades <= kShadowCascadeSlots,
                  "the cascade limit cannot exceed the slots the split array carries");

    // ===== Per-type descriptors (GPU-ready fields, no alignment padding) =====
    struct LUX_TYPE_INFO(runtime) DirectionalLightDesc {
        Eigen::Vector3f LUX_MEMBER(display_name=Direction, tooltip=Light direction normalized) direction{0.f, -1.f, 0.f};
        Eigen::Vector3f LUX_MEMBER(display_name=Color, color=true, tooltip=Light color RGB) color{1.f, 1.f, 1.f};
        float           LUX_MEMBER(display_name=Intensity, min=0.0, max=50.0) intensity{1.f};
        uint32_t        flags{0};
        uint32_t        shadow_map_size{1024};
        float           LUX_MEMBER(display_name=Shadow Bias, min=0.0, max=0.1) shadow_bias{0.005f};
        float           LUX_MEMBER(display_name=Normal Bias, min=0.0, max=0.5) shadow_normal_bias{0.01f};
        uint32_t        cascade_count{4};
        std::array<float, kShadowCascadeSlots> cascade_splits{0.1f, 0.25f, 0.5f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    };

    struct LUX_TYPE_INFO(runtime) PointLightDesc {
        RenderLargePosition3D spatial_position{};
        Eigen::Vector3f LUX_MEMBER(display_name=Color, color=true) color{1.f, 1.f, 1.f};
        float           LUX_MEMBER(display_name=Intensity, min=0.0, max=50.0) intensity{1.f};
        float           LUX_MEMBER(display_name=Range, min=0.1, max=500.0) range{10.f};
        float           attenuation_constant{1.f};
        float           attenuation_linear{0.09f};
        float           attenuation_quadratic{0.032f};
        uint32_t        flags{0};
        uint32_t        shadow_map_size{1024};
        float           LUX_MEMBER(display_name=Shadow Bias, min=0.0, max=0.1) shadow_bias{0.005f};
        float           LUX_MEMBER(display_name=Normal Bias, min=0.0, max=0.5) shadow_normal_bias{0.01f};
    };

    struct LUX_TYPE_INFO(runtime) SpotLightDesc {
        RenderLargePosition3D spatial_position{};
        Eigen::Vector3f LUX_MEMBER(display_name=Direction, tooltip=Spot direction normalized) direction{0.f, -1.f, 0.f};
        Eigen::Vector3f LUX_MEMBER(display_name=Color, color=true) color{1.f, 1.f, 1.f};
        float           LUX_MEMBER(display_name=Intensity, min=0.0, max=50.0) intensity{1.f};
        float           LUX_MEMBER(display_name=Range, min=0.1, max=500.0) range{10.f};
        float           attenuation_constant{1.f};
        float           attenuation_linear{0.09f};
        float           attenuation_quadratic{0.032f};
        float           LUX_MEMBER(display_name=Inner Angle, min=0.0, max=1.5708) inner_cone_angle{0.5236f};  // ~30 deg
        float           LUX_MEMBER(display_name=Outer Angle, min=0.0, max=1.5708) outer_cone_angle{0.7854f};  // ~45 deg
        uint32_t        flags{0};
        uint32_t        shadow_map_size{1024};
        float           LUX_MEMBER(display_name=Shadow Bias, min=0.0, max=0.1) shadow_bias{0.005f};
        float           LUX_MEMBER(display_name=Normal Bias, min=0.0, max=0.5) shadow_normal_bias{0.01f};
    };

    struct AreaLightDesc {
        Eigen::Vector3f color{1.f, 1.f, 1.f};
        float           intensity{1.f};
        Eigen::Vector2f size{1.f, 1.f};
        uint32_t        flags{0};
        uint32_t        shadow_map_size{1024};
        float           shadow_bias{0.005f};
        float           shadow_normal_bias{0.01f};
    };

    /// Render-domain light descriptor — a variant of all supported light types.
    /// Created via lightDescriptorFromComponent() from ECS data,
    /// consumed by LightResources without any ECS dependency.
    using LightDescriptor = std::variant<
        DirectionalLightDesc,
        PointLightDesc,
        SpotLightDesc,
        AreaLightDesc
    >;
} // namespace lux::render
