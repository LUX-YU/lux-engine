#pragma once

#include <lux/engine/render/data/ShaderInterface.hpp>

namespace lux::render
{
    // Cooked SPIR-V and exact entry-point ABI for the current real graphics path.
    // Owned bytes are the identity; no hash-only equality, compiler or parser in this header.
    struct SpirvArtifact final
    {
        std::vector<std::uint32_t> vertex;
        std::vector<std::uint32_t> fragment;
        ShaderInterface interface;
        bool operator==(const SpirvArtifact&) const noexcept = default;
    };

}
