#pragma once

#include <lux/engine/description/GBufferLayout.hpp>
#include <array>
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
}
