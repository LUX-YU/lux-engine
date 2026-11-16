#pragma once

#include "../../material/test/MaterialTest.hpp"
#include <lux/engine/material/MaterialLowering.hpp>
#include <lux/engine/material/compiler/MaterialCompiler.hpp>

inline lux::material::MaterialDeriveResult
deriveFixtureMaterial(
    const lux::material::MaterialGraph& graph,
    std::span<const lux::rdesc::EMaterialPurpose> purposes
) noexcept
{
    auto ir = lux::material::lowerMaterial(graph);
    if (!ir)
    {
        return lux::cxx::unexpected(
            lux::material::MaterialDeriveFailure{lux::material::EMaterialDeriveError::INVALID_IR, ir.error().message}
        );
    }
    std::vector<lux::asset::AssetId> textures;
    for (const auto& slot : graph.texture_slots)
    {
        textures.push_back(slot.texture);
    }
    return lux::material::deriveMaterial(*ir, textures, {lux::rdesc::EShaderTarget::SPIRV, purposes});
}
