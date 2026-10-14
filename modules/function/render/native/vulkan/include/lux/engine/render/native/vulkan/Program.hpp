#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace lux::render::vulkan
{
    // Cooked SPIR-V and exact entry-point ABI for the current real graphics path.
    // Owned bytes are the identity; no hash-only equality, compiler or parser in this header.
    struct ShaderArtifact final
    {
        std::vector<std::uint32_t> vertex;
        std::vector<std::uint32_t> fragment;
        std::uint32_t abi = 1; // no descriptors/vertex inputs, fragment vec4 push constant + location 0 output
        bool operator==(const ShaderArtifact&) const noexcept = default;
    };

    // Cold compiler input, one binding per live Pass. It is the only Shader binding authority.
    // Feature registration uses a local name; assembly's caller expands it with featureKey().
    struct PassProgram final
    {
        std::string pass_key;
        ShaderArtifact shader; // empty for transfer-only commands
        std::array<float, 4> clear_color{0, 0, 0, 1};
        std::uint32_t vertex_count = 3;
    };
}
