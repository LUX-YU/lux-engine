#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/graph/ShaderInterface.hpp>
#include <lux/engine/render/resource/StorageDescriptor.hpp>

namespace lux::render::detail
{
    RenderResult<ShaderInterface> shaderInterface(const rdesc::ShaderInfo& cooked) noexcept
    {
        ShaderInterface result;
        for (const auto& set : cooked.sets)
        {
            for (const auto& source : set.bindings)
            {
                const bool is_buffer = source.type == rdesc::EDescriptorType::STORAGE_BUFFER;
                const bool is_images = source.type == rdesc::EDescriptorType::SAMPLED_IMAGE;
                const bool is_samplers = source.type == rdesc::EDescriptorType::SAMPLER;
                if (!is_buffer && !is_images && !is_samplers)
                {
                    return cxx::unexpected(RenderError{kDeviceUnsupported});
                }
                const auto access = source.writable
                                        ? (source.readable ? EStorageAccess::READ_WRITE : EStorageAccess::WRITE_ONLY)
                                        : EStorageAccess::READ_ONLY;
                result.bindings.push_back(
                    {source.set,
                     source.binding,
                     is_buffer   ? EDescriptorKind::STORAGE_BUFFER
                     : is_images ? EDescriptorKind::SAMPLED_IMAGE_ARRAY
                                 : EDescriptorKind::SAMPLER_ARRAY,
                     source.count,
                     source.block,
                     source.name,
                     source.stages,
                     access}
                );
            }
        }
        for (const auto& source : cooked.push_constants)
        {
            // The current executor consumes one complete push block. Separate or overlapping
            // incompatible blocks are rejected here, not flattened into an invented ABI.
            const bool is_incompatible = source.offset != 0 || source.size != source.block.size ||
                                         (result.parameters.size && result.parameters != source.block);
            if (is_incompatible)
            {
                return cxx::unexpected(RenderError{kStorageContract});
            }
            result.parameters = source.block;
            result.parameter_stages |= source.stages;
        }
        return result;
    }

} // namespace lux::render::detail
