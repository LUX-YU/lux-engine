#pragma once

#include <lux/engine/render/resource/ResourceRequirement.hpp>

#include <lux/engine/render/resource/transform/Data.hpp>

namespace lux::render
{
    inline constexpr ResourceNameView kViewParameterResourceName{"lux.render.resource.view_parameters"};
    inline constexpr ResourceRequirement kViewParameters{
        .binding = "view_parameters", .type = kViewParameterResourceName, .domain = EResourceDomain::VIEW, .scope_default = true
    };

    // Complete shader view block. Matrices are column-major and supplied by the caller.
    // Position/direction are world-space, and viewport is normalized x/y/width/height.
    struct alignas(16) ViewGpuParameters final
    {
        TransformMatrix view = kIdentityTransform;
        TransformMatrix projection = kIdentityTransform;
        TransformMatrix inverse_view_projection = kIdentityTransform;
        std::array<float, 4> position_near{0, 0, 0, 0.1F};
        std::array<float, 4> direction_far{0, 0, -1, 1000};
        std::array<float, 4> viewport{0, 0, 1, 1};
        std::array<float, 4> background{};

        bool operator==(const ViewGpuParameters&) const noexcept = default;
    };
    static_assert(std::is_standard_layout_v<ViewGpuParameters> && std::is_trivially_copyable_v<ViewGpuParameters>);
    static_assert(sizeof(ViewGpuParameters) == 256 && alignof(ViewGpuParameters) == 16);
    static_assert(offsetof(ViewGpuParameters, inverse_view_projection) == 128);
    static_assert(offsetof(ViewGpuParameters, position_near) == 192 && offsetof(ViewGpuParameters, background) == 240);
} // namespace lux::render
