#pragma once

#include <lux/engine/render/resource/Storage.hpp>
#include <memory>

namespace lux::render
{
    enum class EGraphicsApi : std::uint8_t { VULKAN, CONTRACT_TEST };

    // One owned native instance. The factory's matching Ops are separate immutable metadata.
    using NativeStorage = std::unique_ptr<void, void (*)(void*) noexcept>;

    struct StorageFactory final
    {
        const StorageDescriptor* description;
        EGraphicsApi api;
        const void* ops;
        RenderResult<NativeStorage> (*create)(void* device, const void* config) noexcept;
        bool (*compatible_config)(const void* instance, const void* config) noexcept;
        std::shared_ptr<const void> code_pin;
        const void* binding_ops = nullptr; // API-specific immutable preparation functions; never a second owner
        void (*collect)(void*) noexcept = nullptr;
    };

    [[nodiscard]] RenderResult<void>
    validateStorageFactory(const StorageDescriptor& type, const StorageFactory& factory, EGraphicsApi api) noexcept;
}
