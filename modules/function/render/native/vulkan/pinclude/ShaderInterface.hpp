#pragma once

#include <cstdint>
#include <lux/engine/description/ShaderBlock.hpp>
#include <lux/engine/description/ShaderInfo.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <string>
#include <vector>

namespace lux::render::vulkan
{
    enum class EStorageAccess : std::uint8_t
    {
        READ_ONLY,
        WRITE_ONLY,
        READ_WRITE
    };

    enum class EDescriptorKind : std::uint8_t
    {
        SAMPLED_IMAGE_ARRAY,
        STORAGE_BUFFER
    };

    // Native layout projection of cooked reflection. No member offset or stride is inferred here.
    struct ShaderBinding final
    {
        std::uint32_t set = 0, binding = 0;
        EDescriptorKind kind = EDescriptorKind::SAMPLED_IMAGE_ARRAY;
        std::uint32_t count = 0;
        rdesc::ShaderBlock block; // Full owned member/stride ABI for Buffer resources; empty for opaque descriptors.
        std::string name;         // Cooked shader variable name, resolved to a Graph resource only during compilation.
        std::uint32_t stages = 2; // Existing PassContract stage bits: Vertex=1, Fragment=2, Compute=4.
        EStorageAccess access = EStorageAccess::READ_ONLY;
        bool operator==(const ShaderBinding&) const noexcept = default;
    };

    struct ShaderInterface final
    {
        std::vector<ShaderBinding> bindings;
        rdesc::ShaderBlock parameters;
        std::uint32_t parameter_stages = 0;
        bool operator==(const ShaderInterface&) const noexcept = default;
    };

    // Cold projection only. Cooked ShaderInfo remains the public source of ABI facts.
    [[nodiscard]] RenderResult<ShaderInterface> shaderInterface(const rdesc::ShaderInfo&) noexcept;
} // namespace lux::render::vulkan
