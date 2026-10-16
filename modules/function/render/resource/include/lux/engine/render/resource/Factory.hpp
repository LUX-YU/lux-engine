#pragma once

#include <lux/engine/render/resource/Storage.hpp>
#include <memory>

namespace lux::render
{
    struct CommandHandler;

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

    // Cold declaration-to-implementation binding. The factory remains the only
    // native instance owner; the handlers are immutable borrowed metadata.
    struct StorageRegistration final
    {
        // Registration is constructed once. Memberwise replacement would release
        // the old factory code pin before its configuration's foreign deleter.
        StorageRegistration& operator=(const StorageRegistration&) = delete;

        const StorageDescriptor* description;
        std::span<const CommandHandler> handlers;
        StorageFactory factory;
        bool optional = false;
        std::shared_ptr<const void> configuration;
    };

    // The selected Native module supplies a specialization. A declaration does
    // not silently choose a backend or create a second resource directory.
    template <typename T>
    [[nodiscard]] StorageRegistration nativeStorageBinding() noexcept;

    [[nodiscard]] RenderResult<void>
    validateStorageFactory(const StorageDescriptor& type, const StorageFactory& factory, EGraphicsApi api) noexcept;
}
