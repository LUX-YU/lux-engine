#pragma once

#include <NRI.h>

namespace lux::render::detail
{
    inline nri::AccessBits bufferAccess(nri::BufferUsageBits usage) noexcept
    {
        auto access = nri::AccessBits::COPY_SOURCE | nri::AccessBits::COPY_DESTINATION;
        if ((usage & nri::BufferUsageBits::VERTEX_BUFFER) != 0)
        {
            access |= nri::AccessBits::VERTEX_BUFFER;
        }
        if ((usage & nri::BufferUsageBits::INDEX_BUFFER) != 0)
        {
            access |= nri::AccessBits::INDEX_BUFFER;
        }
        if ((usage & nri::BufferUsageBits::CONSTANT_BUFFER) != 0)
        {
            access |= nri::AccessBits::CONSTANT_BUFFER;
        }
        if ((usage & nri::BufferUsageBits::ARGUMENT_BUFFER) != 0)
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
