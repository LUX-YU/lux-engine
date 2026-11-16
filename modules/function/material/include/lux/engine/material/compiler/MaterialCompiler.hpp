#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/description/Material.hpp>
#include <lux/engine/material/MaterialIR.hpp>
#include <lux/engine/material/compiler/visibility.h>

namespace lux::material
{
    enum class EMaterialDeriveError : std::uint8_t
    {
        INVALID_IR,
        INVALID_REQUEST,
        UNSUPPORTED_SOURCE,
        COMPILER_FAILURE,
        INVALID_LAYOUT
    };

    struct MaterialDeriveFailure final
    {
        EMaterialDeriveError code;
        std::string diagnostic;
    };

    struct MaterialDeriveOptions final
    {
        rdesc::EShaderTarget target;
        std::span<const rdesc::EMaterialPurpose> purposes;
        std::string_view profile = "sm_6_6";
    };

    struct MaterialDerivation final
    {
        rdesc::MaterialDescription material;
        // Code/layout key. Runtime defaults and texture asset bindings are returned above, never keyed as code.
        std::uint64_t program_cache_key = 0;
    };

    using MaterialDeriveResult = cxx::expected<MaterialDerivation, MaterialDeriveFailure>;

    [[nodiscard]] LUX_MATERIAL_COMPILER_PUBLIC MaterialDeriveResult deriveMaterial(
        const MaterialIR& ir,
        std::span<const asset::AssetId> textures,
        const MaterialDeriveOptions& options
    ) noexcept;
} // namespace lux::material
