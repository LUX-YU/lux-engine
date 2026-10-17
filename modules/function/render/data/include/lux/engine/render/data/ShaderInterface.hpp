#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lux::render
{
    enum class EDescriptorKind : std::uint8_t { SAMPLED_IMAGE_ARRAY, STORAGE_BUFFER };

    // Cooked descriptor reflection facts. Source locations are provisional; Native LayoutPlan owns
    // final placement. Zero count denotes a runtime array bounded by actual device capacity.
    struct ShaderBinding final
    {
        std::uint32_t set = 0, binding = 0;
        EDescriptorKind kind = EDescriptorKind::SAMPLED_IMAGE_ARRAY;
        std::uint32_t count = 0;
        std::uint32_t layout_bytes = 0; // current StorageBuffer primitive: one float4 at offset zero (16 bytes)
        std::string name; // Cooked shader variable name, resolved to a Graph resource only during compilation.
        std::uint32_t stages = 2; // Existing PassContract stage bits: Vertex=1, Fragment=2, Compute=4.
        bool operator==(const ShaderBinding&) const noexcept = default;
    };

    struct ShaderInterface final
    {
        std::vector<ShaderBinding> bindings;
        std::uint32_t push_bytes = 16;
        bool operator==(const ShaderInterface&) const noexcept = default;
    };
}
