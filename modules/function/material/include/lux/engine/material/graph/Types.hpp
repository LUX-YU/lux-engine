#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>

namespace lux::material
{
    enum class EValueType : std::uint8_t
    {
        FLOAT,
        VEC2,
        VEC3,
        VEC4
    };

    enum class EMaterialAttribute : std::uint8_t
    {
        BASE_COLOR = 0,
        OPACITY,
        METALLIC,
        ROUGHNESS,
        NORMAL_TS,
        EMISSIVE,
        AMBIENT_OCCLUSION,
        COUNT
    };

    struct MaterialAttributeDesc final
    {
        EMaterialAttribute attribute;
        const char* name;
        EValueType type;
        float dflt[4];
    };

    inline constexpr MaterialAttributeDesc kMaterialAttributes[] = {
        {EMaterialAttribute::BASE_COLOR, "base_color", EValueType::VEC3, {1.0F, 1.0F, 1.0F, 0.0F}},
        {EMaterialAttribute::OPACITY, "opacity", EValueType::FLOAT, {1.0F, 0.0F, 0.0F, 0.0F}},
        {EMaterialAttribute::METALLIC, "metallic", EValueType::FLOAT, {0.0F, 0.0F, 0.0F, 0.0F}},
        {EMaterialAttribute::ROUGHNESS, "roughness", EValueType::FLOAT, {1.0F, 0.0F, 0.0F, 0.0F}},
        {EMaterialAttribute::NORMAL_TS, "normal_ts", EValueType::VEC3, {0.0F, 0.0F, 1.0F, 0.0F}},
        {EMaterialAttribute::EMISSIVE, "emissive", EValueType::VEC3, {0.0F, 0.0F, 0.0F, 0.0F}},
        {EMaterialAttribute::AMBIENT_OCCLUSION, "ao", EValueType::FLOAT, {1.0F, 0.0F, 0.0F, 0.0F}}
    };

    static_assert(std::size(kMaterialAttributes) == static_cast<std::size_t>(EMaterialAttribute::COUNT));

    enum class EMaterialInput : std::uint8_t
    {
        UV0 = 0,
        WORLD_POSITION,
        WORLD_NORMAL,
        WORLD_TANGENT,
        VERTEX_COLOR,
        COUNT
    };

    struct MaterialInputDesc final
    {
        EMaterialInput input;
        const char* name;
        EValueType type;
    };

    inline constexpr MaterialInputDesc kMaterialInputs[] = {
        {EMaterialInput::UV0, "uv0", EValueType::VEC2},
        {EMaterialInput::WORLD_POSITION, "world_position", EValueType::VEC3},
        {EMaterialInput::WORLD_NORMAL, "world_normal", EValueType::VEC3},
        {EMaterialInput::WORLD_TANGENT, "world_tangent", EValueType::VEC4},
        {EMaterialInput::VERTEX_COLOR, "vertex_color", EValueType::VEC4}
    };

    static_assert(std::size(kMaterialInputs) == static_cast<std::size_t>(EMaterialInput::COUNT));

    [[nodiscard]] inline constexpr EValueType attributeType(EMaterialAttribute attribute) noexcept
    {
        return kMaterialAttributes[static_cast<std::size_t>(attribute)].type;
    }

    [[nodiscard]] inline constexpr EValueType inputType(EMaterialInput input) noexcept
    {
        return kMaterialInputs[static_cast<std::size_t>(input)].type;
    }
} // namespace lux::material
