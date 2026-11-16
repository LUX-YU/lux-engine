#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/description/Material.hpp>
#include <lux/engine/material/compiler/visibility.h>
#include <lux/engine/material/graph/MaterialGraph.hpp>
#include <lux/engine/shader/ShaderCompiler.hpp>

namespace lux::material
{
    enum class EMaterialDeriveError : std::uint8_t
    {
        INVALID_GRAPH,
        INVALID_REQUEST,
        COMPILER_FAILURE,
        INVALID_LAYOUT
    };

    struct MaterialDeriveFailure final
    {
        EMaterialDeriveError code;
        std::string diagnostic;
        graph::NodeId node_id{};
        std::uint32_t pin_index = ~0u;
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
        std::string diagnostic;
    };

    using MaterialDeriveResult = cxx::expected<MaterialDerivation, MaterialDeriveFailure>;

    [[nodiscard]] LUX_MATERIAL_COMPILER_PUBLIC MaterialNodeResult<std::string>
    generateMaterialSlang(const MaterialGraph&, rdesc::EMaterialPurpose) noexcept;

    [[nodiscard]] LUX_MATERIAL_COMPILER_PUBLIC MaterialDeriveResult
    deriveMaterial(
        shader::ShaderCompiler& compiler,
        const MaterialGraph& graph,
        const MaterialDeriveOptions& options
    ) noexcept;
} // namespace lux::material
