#pragma once

#include <NRI.h>
#include <cstdint>
#include <lux/engine/description/ShaderBlock.hpp>
#include <lux/engine/description/ShaderInfo.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <string>
#include <vector>

namespace lux::render::detail
{
    enum class EShaderAccess : std::uint8_t
    {
        READ_ONLY,
        WRITE_ONLY,
        READ_WRITE
    };

    enum class EDescriptorKind : std::uint8_t
    {
        SAMPLED_IMAGE,
        SAMPLER,
        STORAGE_BUFFER,
        UNIFORM_BUFFER,
        STORAGE_IMAGE,
        TYPED_STORAGE_BUFFER
    };

    // Native layout projection of cooked reflection. No member offset or stride is inferred here.
    struct ShaderBinding final
    {
        std::uint32_t set = 0, binding = 0;
        EDescriptorKind kind = EDescriptorKind::SAMPLED_IMAGE;
        std::uint32_t count = 0;
        rdesc::ShaderBlock block; // Full owned member/stride ABI for Buffer resources; empty for opaque descriptors.
        std::string name;         // Cooked shader variable name, resolved to a Graph resource only during compilation.
        std::uint32_t stages = 2; // Existing PassContract stage bits: Vertex=1, Fragment=2, Compute=4.
        EShaderAccess access = EShaderAccess::READ_ONLY;
        bool image_arrayed = false, image_depth = false;
        // Binary register identity survives compact descriptor placement. DXIL is never patched.
        std::uint32_t shader_space = 0, shader_register = 0;
        bool operator==(const ShaderBinding&) const noexcept = default;

        [[nodiscard]] nri::BufferViewDesc
        bufferView(const nri::Buffer* buffer, std::uint64_t offset, std::uint64_t size) const noexcept
        {
            const bool is_read_only = access == EShaderAccess::READ_ONLY;
            auto type = block.size ? (is_read_only ? nri::BufferView::STRUCTURED_BUFFER
                                                   : nri::BufferView::STORAGE_STRUCTURED_BUFFER)
                                   : (is_read_only ? nri::BufferView::BYTE_ADDRESS_BUFFER
                                                   : nri::BufferView::STORAGE_BYTE_ADDRESS_BUFFER);
            if (kind == EDescriptorKind::UNIFORM_BUFFER)
            {
                type = nri::BufferView::CONSTANT_BUFFER;
            }
            if (kind == EDescriptorKind::TYPED_STORAGE_BUFFER)
            {
                type = nri::BufferView::STORAGE_BUFFER;
            }
            const bool is_structured =
                type == nri::BufferView::STRUCTURED_BUFFER || type == nri::BufferView::STORAGE_STRUCTURED_BUFFER;
            return {
                buffer,
                type,
                offset,
                size,
                kind == EDescriptorKind::TYPED_STORAGE_BUFFER ? nri::Format::R32_UINT : nri::Format::UNKNOWN,
                is_structured ? block.size : 0
            };
        }
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
} // namespace lux::render::detail
