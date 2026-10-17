#pragma once

#include <lux/engine/render/resource/Storage.hpp>
#include <memory>
#include <concepts>
#include <lux/cxx/compile_time/TypeToken.hpp>

namespace lux::render
{
    // One owned native instance. The factory's matching Ops are separate immutable metadata.
    using NativeStorage = std::unique_ptr<void, void (*)(void*) noexcept>;

    struct StorageCppType final
    {
        cxx::TypeToken type;
        std::size_t size = 0;
        std::size_t alignment = 0;
        bool operator==(const StorageCppType&) const noexcept = default;
    };

    template <class T>
    [[nodiscard]] constexpr StorageCppType storageCppType() noexcept
    {
        return {cxx::typeToken<T>(), sizeof(T), alignof(T)};
    }

    namespace resource_detail { struct StorageFactoryAudit; }

    struct StorageFactory final
    {
        const StorageDescriptor* description;
        const void* ops;
        RenderResult<NativeStorage> (*create)(void* device, StorageId id, const void* config) noexcept;
        bool (*compatible_config)(const void* instance, const void* config) noexcept;
        std::shared_ptr<const void> code_pin;
        const void* binding_ops = nullptr; // API-specific immutable preparation functions; never a second owner
        StorageCppType config_type;
        StorageCppType ops_type;
        StorageCppType native_type;

    private:
        friend struct resource_detail::StorageFactoryAudit;
        template <class Declaration, class Native, class Device, auto Create, auto Compatible>
            requires std::same_as<decltype(Create), RenderResult<std::unique_ptr<Native>> (*)(
                Device&, StorageId, const typename Declaration::Config&) noexcept> &&
                std::same_as<decltype(Compatible), bool (*)(const Native&, const typename Declaration::Config&) noexcept>
        friend StorageFactory makeStorageFactory(
            const typename Declaration::Ops&, const void*, std::shared_ptr<const void>
        ) noexcept;

        StorageFactory(const StorageDescriptor* description, const void* ops,
            RenderResult<NativeStorage> (*create)(void*, StorageId, const void*) noexcept,
            bool (*compatible_config)(const void*, const void*) noexcept,
            std::shared_ptr<const void> code_pin, const void* binding_ops,
            StorageCppType config_type, StorageCppType ops_type, StorageCppType native_type) noexcept
            : description(description), ops(ops), create(create), compatible_config(compatible_config),
              code_pin(std::move(code_pin)), binding_ops(binding_ops),
              config_type(config_type), ops_type(ops_type), native_type(native_type) {}

    };

    // Create and compatible callbacks are typed before the only erasure boundary.
    template <class Declaration, class Native, class Device, auto Create, auto Compatible>
        requires std::same_as<decltype(Create), RenderResult<std::unique_ptr<Native>> (*)(
            Device&, StorageId, const typename Declaration::Config&) noexcept> &&
            std::same_as<decltype(Compatible), bool (*)(const Native&, const typename Declaration::Config&) noexcept>
    [[nodiscard]] StorageFactory makeStorageFactory(
        const typename Declaration::Ops& ops, const void* binding_ops = nullptr,
        std::shared_ptr<const void> code_pin = {}
    ) noexcept
    {
        return {&Declaration::description, &ops,
            [](void* device, StorageId id, const void* config) noexcept -> RenderResult<NativeStorage> {
                auto candidate = Create(
                    *static_cast<Device*>(device), id, *static_cast<const typename Declaration::Config*>(config)
                );
                if (!candidate) return cxx::unexpected(candidate.error());
                return NativeStorage{candidate->release(), [](void* value) noexcept { delete static_cast<Native*>(value); }};
            }, [](const void* instance, const void* config) noexcept {
                return Compatible(*static_cast<const Native*>(instance), *static_cast<const typename Declaration::Config*>(config));
            }, std::move(code_pin), binding_ops,
            storageCppType<typename Declaration::Config>(), storageCppType<typename Declaration::Ops>(),
            storageCppType<Native>()};
    }

    [[nodiscard]] RenderResult<void>
    validateStorageFactory(const StorageDescriptor& type, const StorageFactory& factory) noexcept;
}
