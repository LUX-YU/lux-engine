#pragma once

#include <lux/engine/render/capabilities/ImguiCapability.hpp>
#include <lux/engine/description/ShaderBlock.hpp>

#include <lux/engine/render/core/ConfigValue.hpp>
#include <lux/engine/render/inputs/DrawInputs.hpp>
#include <lux/engine/render/inputs/ImageShape.hpp>
#include <lux/engine/render/resources/texture/Data.hpp>
#include <algorithm>
#include <cstddef>
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

        // Qualify these CPU field offsets against each target; reflected trailing padding is allowed.
        [[nodiscard]] static bool matchesLayout(const rdesc::ShaderBlock& layout) noexcept
        {
            if (layout.size < sizeof(UiDrawParameters) || layout.fields.size() != 3) return false;
            const std::array<std::string_view, 3> names{"scale", "translation", "source_index"};
            const std::array<std::uint32_t, 3> offsets{
                offsetof(UiDrawParameters, scale),
                offsetof(UiDrawParameters, translation),
                offsetof(UiDrawParameters, source_index)
            };
            for (std::size_t i = 0; i < names.size(); ++i)
            {
                const auto field = std::ranges::find(layout.fields, names[i], &rdesc::ShaderMember::path);
                if (field == layout.fields.end()) return false;
                const bool is_invalid_field = field->offset != offsets[i] || field->columns != 1 ||
                    field->rows != (i == 2 ? 1u : 2u) || field->array_stride != 0 ||
                    field->scalar != (i == 2 ? rdesc::EDataScalar::UINT : rdesc::EDataScalar::FLOAT);
                if (is_invalid_field) return false;
            }
            return true;
        }
    };

} // namespace lux::render
