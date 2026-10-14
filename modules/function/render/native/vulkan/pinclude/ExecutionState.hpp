#pragma once

#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>

namespace lux::render::vulkan
{
    struct ScopedBuffer final
    {
        std::string key;
        StorageRequirement requirement;
        ScopeIdentity scope;
        const ExecutionContext* attachment = nullptr;
        BufferAllocation* backing;
        std::size_t leases = 0;
        bool live_scope = false;
    };
}
