#pragma once

#include <array>
#include <lux/engine/description/GBufferLayout.hpp>
#include <lux/engine/render/effect/EffectNodeDefinition.hpp>
#include <lux/engine/render/visibility.h>

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

    namespace GBuffer
    {
        inline constexpr std::string_view capability = "lux.render.gbuffer";
        inline constexpr std::string_view indexed = "lux.render.gbuffer.indexed";
        inline constexpr std::string_view indexed_clockwise = "lux.render.gbuffer.indexed.clockwise";
        inline constexpr std::uint32_t version = 1;
        inline constexpr auto surface = effectPortSemantic("surface");
        inline constexpr auto draws = effectPortSemantic("draws");
        inline constexpr std::string_view geometry_binding = "mesh.geometry";
        inline constexpr std::string_view materials_binding = "mesh.materials";
        inline constexpr std::string_view textures_binding = "mesh.textures";

        [[nodiscard]] LUX_RENDER_PUBLIC EffectNodeDefinition definition() noexcept;
    }
}
