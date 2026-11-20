#pragma once

#include <lux/engine/render/effect/CapabilityDefinition.hpp>
#include <lux/engine/render/visibility.h>

namespace lux::render
{
    struct ToneMapping final
    {
        static constexpr std::string_view capability = "lux.render.tone_mapping";
        static constexpr std::string_view linear = "lux.render.tone_mapping.linear";
        static constexpr std::string_view srgb = "lux.render.tone_mapping.srgb";
        static constexpr std::uint32_t version = 1;
        static constexpr auto hdr_color = effectPortSemantic("hdr_color");
        static constexpr auto color = effectPortSemantic("color");
        static constexpr std::string_view hdr_type = "lux.color.linear-hdr";
        static constexpr std::string_view display_type = "lux.color.display";

        [[nodiscard]] LUX_RENDER_PUBLIC static CapabilityDefinition definition() noexcept;
    };
} // namespace lux::render
