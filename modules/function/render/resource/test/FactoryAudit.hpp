#pragma once

#include <lux/engine/render/resource/Factory.hpp>

namespace lux::render::resource_detail
{
    // Explicitly trusted malformed-factory boundary used only by historical negative/cost fixtures.
    struct StorageFactoryAudit final
    {
        static StorageFactory make(const StorageDescriptor* description, const void* ops,
            RenderResult<NativeStorage> (*create)(void*, StorageId, const void*) noexcept,
            bool (*compatible)(const void*, const void*) noexcept,
            const void* bindings,
            CppType config_type, CppType ops_type, CppType native_type = {}) noexcept
        {
            return {description, ops, create, compatible, bindings, config_type, ops_type, native_type};
        }
    };
}
