#pragma once

#include <string_view>

#include <lux/engine/material/MaterialMath.hpp>
#include <lux/engine/material/MaterialNodeCatalog.hpp>

#include <array>
#include <cstdint>

namespace lux::material
{
    // Semantic payloads only. Pin identities, links and editable defaults belong to the graph stores.
    struct MaterialConstant final
    {
        static constexpr std::string_view TypeName{"lux.material.constant.v1"};
        std::array<float, 4> value{};
        EValueType type{EValueType::VEC4};
    };

    struct MaterialInput final
    {
        static constexpr std::string_view TypeName{"lux.material.input.v1"};
        EMaterialInput input{EMaterialInput::UV0};
    };

    struct MaterialSampleTexture final
    {
        static constexpr std::string_view TypeName{"lux.material.sample_texture.v1"};
        std::uint32_t texture_slot{};
    };

    struct MaterialParameter final
    {
        static constexpr std::string_view TypeName{"lux.material.parameter.v1"};
        std::uint32_t param_slot{};
        EValueType type{EValueType::VEC4};
    };

    struct MaterialSwizzle final
    {
        static constexpr std::string_view TypeName{"lux.material.swizzle.v1"};
        EValueType source_type{EValueType::VEC4};
        EValueType out_type{EValueType::VEC3};
        std::array<std::uint8_t, 4> components{0, 1, 2, 3};
    };

    struct MaterialConstruct final
    {
        static constexpr std::string_view TypeName{"lux.material.construct.v1"};
        EValueType out_type{EValueType::VEC3};
    };

    struct MaterialDecodeNormal final
    {
        static constexpr std::string_view TypeName{"lux.material.decode_normal.v1"};
    };

    struct MaterialTbnTransform final
    {
        static constexpr std::string_view TypeName{"lux.material.tbn_transform.v1"};
    };

    struct MaterialOutputSurface final
    {
        static constexpr std::string_view TypeName{"lux.material.output_surface.v1"};
    };

    // Explicit module contribution; does not install a process-global catalog.
    [[nodiscard]] LUX_ENGINE_MATERIAL_GRAPH_PUBLIC std::array<MaterialNodeRegistration, 10>
    materialBuiltinRegistrations() noexcept;
} // namespace lux::material
