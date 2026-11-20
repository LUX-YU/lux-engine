#pragma once

#include <lux/engine/render/effect/CapabilityDefinition.hpp>
#include <lux/engine/render/visibility.h>

namespace lux::render
{
    struct Shadow final
    {
        static constexpr std::string_view capability = "lux.render.shadow";
        static constexpr std::string_view indexed = "lux.render.shadow.indexed";
        static constexpr std::string_view indexed_clockwise = "lux.render.shadow.indexed.clockwise";
        static constexpr std::string_view lights_binding = "shadow.lights";
        static constexpr std::uint32_t version = 1;
        static constexpr auto shadows = effectPortSemantic("shadows");
        static constexpr auto draws = effectPortSemantic("draws");

        [[nodiscard]] LUX_RENDER_PUBLIC static CapabilityDefinition definition() noexcept;
    };
} // namespace lux::render
