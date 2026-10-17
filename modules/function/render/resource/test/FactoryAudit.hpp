#pragma once

#include <lux/engine/render/resource/Factory.hpp>

namespace lux::render::resource_detail
{
    // Explicitly trusted malformed-factory boundary used only by historical negative/cost fixtures.
    struct StorageFactoryAudit final
    {
        static StorageFactory make(const StorageDescriptor* description, const void* ops,
            RenderResult<NativeStorage> (*create)(void*, const void*) noexcept,
            bool (*compatible)(const void*, const void*) noexcept,
            std::shared_ptr<const void> pin, const void* bindings, void (*collect)(void*) noexcept,
            StorageCppType config_type, StorageCppType ops_type) noexcept
        {
            return {description, ops, create, compatible, std::move(pin), bindings, collect, config_type, ops_type, {}};
        }
    };
}
