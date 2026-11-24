#pragma once

#include <lux/engine/render/graph/GraphResource.hpp>
#include <array>

namespace lux::render
{
    // One equal-resolution slice per array layer. Explicit quality; no automatic shrinking or dropping lights.
    struct ShadowMapShape final
    {
        std::uint32_t resolution = 1024, view_count = 1;
        bool operator==(const ShadowMapShape&) const noexcept = default;

        [[nodiscard]] GraphImageDesc image() const noexcept
        {
            GraphImageDesc result{resolution, resolution, 1, 1, rdesc::ETextureFormat::D32_SFLOAT};
            result.runtime_layers = true;
            return result;
        }
    };

    struct ShadowSliceGpu final
    {
        std::array<float, 4> uv_scale_bias{1, 1, 0, 0};
        std::array<float, 4> inner_uv{};
        // near/far view distance for CSM, light near/far otherwise.
        std::array<float, 4> depths{};
        // normal offset in world units, comparison offset in normalized depth, PCF radius in texels, reserved.
        std::array<float, 4> sampling{};
        std::array<std::uint32_t, 4> identity{}; // layer, active, perspective, reserved.
    };

    struct LightShadowRange final
    {
        std::uint32_t first = 0, count = 0, kind = 0, reserved = 0;
    };

    struct ShadowMappingHeader final
    {
        std::array<std::uint32_t, 4> offsets{}; // directional, point, spot, total dense ranges.
        std::array<float, 4> camera_position_near{}, camera_direction_far{};
    };

    static_assert(sizeof(ShadowSliceGpu) == 80);
    static_assert(sizeof(LightShadowRange) == 16 && sizeof(ShadowMappingHeader) == 48);
    inline constexpr std::string_view kShadowSlices = "lux.shadow.slices.forward-z.v1";
    inline constexpr std::string_view kLightShadowRanges = "lux.shadow.dense-light-ranges.v1";
    inline constexpr std::string_view kShadowDepth = "lux.shadow.depth-array.forward-z.v1";
} // namespace lux::render
