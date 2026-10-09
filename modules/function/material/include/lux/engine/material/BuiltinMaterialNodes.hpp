#pragma once

#include <lux/engine/material/MaterialMath.hpp>
#include <lux/engine/material/MaterialNodeCatalog.hpp>

#include <array>
#include <cstdint>

namespace lux::material
{
    // Semantic payloads only. Pin identities, links and editable defaults belong to the graph stores.
    struct MaterialConstant final
    {
        std::array<float, 4> value{};
        EValueType type{EValueType::VEC4};
    };

    struct MaterialInput final
    {
        EMaterialInput input{EMaterialInput::UV0};
    };

    struct MaterialSampleTexture final
    {
        std::uint32_t texture_slot{};
    };

    struct MaterialParameter final
    {
        std::uint32_t param_slot{};
        EValueType type{EValueType::VEC4};
    };

    struct MaterialSwizzle final
    {
        EValueType source_type{EValueType::VEC4};
        EValueType out_type{EValueType::VEC3};
        std::array<std::uint8_t, 4> components{0, 1, 2, 3};
    };

    struct MaterialConstruct final
    {
        EValueType out_type{EValueType::VEC3};
    };

    struct MaterialDecodeNormal final
    {
    };

    struct MaterialTbnTransform final
    {
    };

    struct MaterialOutputSurface final
    {
    };

    // Explicit module contribution; does not install a process-global catalog.
    [[nodiscard]] LUX_ENGINE_MATERIAL_GRAPH_PUBLIC std::array<MaterialNodeRegistration, 10>
    materialBuiltinRegistrations() noexcept;
} // namespace lux::material
