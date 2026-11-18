#pragma once

#include "../../material/test/MaterialTest.hpp"
#include <lux/engine/material/compiler/MaterialCompiler.hpp>

inline lux::material::MaterialDeriveResult deriveFixtureMaterial(
    const lux::material::MaterialGraph& graph,
    std::span<const lux::rdesc::EMaterialPurpose> purposes,
    lux::rdesc::EShaderTarget target = lux::rdesc::EShaderTarget::SPIRV
) noexcept
{
    auto compiler = lux::shader::ShaderCompiler::create();
    if (!compiler)
    {
        return lux::cxx::unexpected(lux::material::MaterialDeriveFailure{
            lux::material::EMaterialDeriveError::COMPILER_FAILURE,
            compiler.error().diagnostic
        });
    }
    return lux::material::deriveMaterial(**compiler, graph, {target, purposes});
}
