#pragma once

#include <lux/engine/render/capabilities/ImguiCapability.hpp>

#include <lux/engine/render/core/ConfigValue.hpp>
#include <lux/engine/render/inputs/DrawInputs.hpp>
#include <lux/engine/render/inputs/ImageShape.hpp>
#include <lux/engine/render/inputs/SampledTextureInputs.hpp>
#include <variant>
#include <vector>

namespace lux::render
{
    inline constexpr std::string_view kUiSceneCapability = cxx::make_basic_type_info<ImguiCapability>().name();

    struct UiConfiguration final
    {
        EIndexType index_type = EIndexType::UINT16;
        std::array<float, 4> clear{0.015F, 0.015F, 0.015F, 1}; // Linear BT.709; alpha is coverage.
        bool operator==(const UiConfiguration&) const noexcept = default;
    };

    struct UiDrawParameters final
    {
        std::array<float, 2> scale, translation;
        std::uint32_t source_index = 0;
    };

} // namespace lux::render
