#pragma once

#include <cstdint>
#include <vector>

namespace lux::render
{
    enum class EDescriptorKind : std::uint8_t { SAMPLED_IMAGE_ARRAY, STORAGE_BUFFER };

    // Fragment-stage descriptor contract of the current cooked graphics path. A zero count denotes
    // a runtime array; its physical capacity is chosen by the registered Storage configuration.
    struct ShaderBinding final
    {
        std::uint32_t set = 0, binding = 0;
        EDescriptorKind kind = EDescriptorKind::SAMPLED_IMAGE_ARRAY;
        std::uint32_t count = 0;
        std::uint32_t layout_bytes = 0; // current StorageBuffer primitive: one float4 at offset zero (16 bytes)
        bool operator==(const ShaderBinding&) const noexcept = default;
    };

    struct ShaderInterface final
    {
        std::vector<ShaderBinding> bindings;
        std::uint32_t push_bytes = 16;
        bool operator==(const ShaderInterface&) const noexcept = default;
    };
}
