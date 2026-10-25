#pragma once

#include <lux/engine/description/LightDescription.hpp>
#include <lux/engine/render/inputs/DirectLightInputs.hpp>

namespace lux::scene::detail
{
    // Runtime-derived, not a registered author schema. Scene serialization/Inspector/Play snapshots
    // never include this association. Its destructor performs no asynchronous operation.
    // Only values consumed by the cold shadow recipe. Radiance, position, attenuation and cones
    // remain in author components and go directly into the current FrameInput.
    struct LightShadowInputs final
    {
        bool enabled = false;
        float bias = 0, normal_bias = 0;
        std::uint32_t cascade_count = 0;
        std::array<float, rdesc::kLightCascadeSlots> cascade_splits{};
        bool operator==(const LightShadowInputs&) const noexcept = default;
    };

    struct LightRenderResourceComponent final
    {
        render::LightId light;
        rdesc::ELightType type = rdesc::ELightType::POINT;
        LightShadowInputs shadows;
    };
} // namespace lux::scene::detail
