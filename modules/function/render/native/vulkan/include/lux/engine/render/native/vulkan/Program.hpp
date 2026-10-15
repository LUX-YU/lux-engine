#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <lux/engine/render/features/Texture.hpp>

namespace lux::render::vulkan
{
    // Cooked SPIR-V and exact entry-point ABI for the current real graphics path.
    // Owned bytes are the identity; no hash-only equality, compiler or parser in this header.
    struct ShaderArtifact final
    {
        std::vector<std::uint32_t> vertex;
        std::vector<std::uint32_t> fragment;
        // 1: push-color. 2: one Global combined sampler array, same vec4 push extent (slot, color mode, LOD, 0).
        std::uint32_t abi = 1;
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
        TextureId texture{}; // ABI2's single declared SAMPLED input, resolved only when preparing a Context
        float texture_lod = 0;
    };
}
