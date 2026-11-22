#pragma once

#include <NRI.h>

namespace lux::render::detail
{
    class DeviceSession;

    [[nodiscard]] nri::Result createGraphBufferView(DeviceSession&, nri::BufferViewDesc, nri::Descriptor*&) noexcept;

    inline nri::StageBits zeroBufferStages(nri::GraphicsAPI backend) noexcept
    {
        // NRI uses CopyBufferRegion on D3D12 and vkCmdFillBuffer on Vulkan.
        return backend == nri::GraphicsAPI::D3D12 ? nri::StageBits::COPY
                                                  : nri::StageBits::COPY | nri::StageBits::CLEAR_STORAGE;
    }

    inline nri::AccessBits bufferAccess(nri::BufferUsageBits usage) noexcept
    {
        auto access = nri::AccessBits::COPY_SOURCE | nri::AccessBits::COPY_DESTINATION;
        if ((usage & nri::BufferUsageBits::VERTEX) != 0)
        {
            access |= nri::AccessBits::VERTEX_BUFFER;
        }
        if ((usage & nri::BufferUsageBits::INDEX) != 0)
        {
            access |= nri::AccessBits::INDEX_BUFFER;
        }
        if ((usage & nri::BufferUsageBits::CONSTANT) != 0)
        {
            access |= nri::AccessBits::CONSTANT_BUFFER;
        }
        if ((usage & nri::BufferUsageBits::ARGUMENT) != 0)
        {
            access |= nri::AccessBits::ARGUMENT_BUFFER;
        }
        if ((usage & nri::BufferUsageBits::SHADER_RESOURCE) != 0)
        {
            access |= nri::AccessBits::SHADER_RESOURCE;
        }
        if ((usage & nri::BufferUsageBits::SHADER_RESOURCE_STORAGE) != 0)
        {
            access |= nri::AccessBits::SHADER_RESOURCE_STORAGE;
        }
        return access;
    }

} // namespace lux::render::detail
