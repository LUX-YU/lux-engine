#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <lux/engine/description/GBufferLayout.hpp>
#include <string_view>

namespace lux::render
{
    struct GBufferBundle final
    {
        static constexpr auto type = rdesc::GBufferLayout::semantic;
        static constexpr std::uint32_t version = 1;
        static constexpr std::array<std::string_view, 4> members{
            "albedo_metallic", "normal_roughness", "emissive_ao", "depth"
        };

        static constexpr auto formats = []() noexcept
        {
            std::array<rdesc::ETextureFormat, members.size()> result{};
            for (std::size_t index = 0; index < rdesc::GBufferLayout::color_formats.size(); ++index)
            {
                result[index] = rdesc::GBufferLayout::color_formats[index];
            }
            result.back() = rdesc::GBufferLayout::depth_format;
            return result;
        }();
        static_assert(members.size() == rdesc::GBufferLayout::color_formats.size() + 1);
    };

    struct ShadowBundle final
    {
        static constexpr std::string_view type = "lux.lighting.shadow.forward-z";
        static constexpr std::uint32_t version = 1;
        static constexpr std::array<std::string_view, 4> members{
            "shadow_views", "shadow_slices", "shadow_ranges", "shadow_depth"
        };
    };

    struct HdrColorBundle final
    {
        static constexpr std::string_view type = "lux.color.linear-hdr";
        static constexpr std::uint32_t version = 1;
    };

    struct ClusterListBundle final
    {
        static constexpr std::string_view type = "lux.lighting.cluster-lists.forward-z";
        static constexpr std::uint32_t version = 1;
        static constexpr std::string_view grid = "grid";
        static constexpr std::string_view bounds = "bounds";
        static constexpr std::string_view counts = "counts";
        static constexpr std::string_view offsets = "offsets";
        static constexpr std::string_view indices = "indices";
        static constexpr std::string_view status = "status";
        static constexpr std::array members{grid, bounds, counts, offsets, indices, status};
    };
} // namespace lux::render
