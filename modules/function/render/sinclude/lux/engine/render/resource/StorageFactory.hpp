#pragma once

#include <lux/engine/render/visibility.h>

#include <concepts>
#include <lux/engine/render/core/CppType.hpp>
#include <lux/engine/render/resource/StorageDescriptor.hpp>
#include <memory>

namespace lux::render
{
    // One owned native instance. The factory's matching Ops are separate immutable metadata.
    using StorageInstance = std::unique_ptr<void, void (*)(void*) noexcept>;

    template <class Declaration> [[nodiscard]] constexpr CppType storageOpsType() noexcept
    {
        if constexpr (std::is_void_v<typename Declaration::Ops>)
        {
            return {};
        }
        else
        {
            return cppType<typename Declaration::Ops>();
        }
    }

    template <class Ops>
    using StorageOpsArgument =
        std::conditional_t<std::is_void_v<Ops>, std::nullptr_t, std::add_lvalue_reference_t<std::add_const_t<Ops>>>;

    struct StorageFactory final
    {
        const StorageDescriptor* const descriptor;
        const void* const ops;
        RenderResult<StorageInstance> (*const create)(void* device, StorageId id, const void* config) noexcept;
        bool (*const compatible_config)(const void* instance, const void* config) noexcept;
        const void* const binding_ops; // API-specific immutable preparation functions; never a second owner
        const CppType config_type;
        const CppType ops_type;
        const CppType native_type;

    private:
        template <class Declaration, class Native, class Device, auto Create, auto Compatible>
        friend StorageFactory makeStorageFactory(StorageOpsArgument<typename Declaration::Ops>, const void*) noexcept;

        StorageFactory(
            const StorageDescriptor* descriptor,
            const void* ops,
            RenderResult<StorageInstance> (*create)(void*, StorageId, const void*) noexcept,
            bool (*compatible_config)(const void*, const void*) noexcept,
            const void* binding_ops,
            CppType config_type,
            CppType ops_type,
            CppType native_type
        ) noexcept
            : descriptor(descriptor), ops(ops), create(create), compatible_config(compatible_config),
              binding_ops(binding_ops), config_type(config_type), ops_type(ops_type), native_type(native_type)
        {
        }
    };

    // Create and compatible callbacks are typed before the only erasure boundary.
    template <class Declaration, class Native, class Device, auto Create, auto Compatible = nullptr>
    [[nodiscard]] StorageFactory makeStorageFactory(
        StorageOpsArgument<typename Declaration::Ops> ops,
        const void* binding_ops = nullptr
    ) noexcept
    {
        using Config = typename Declaration::Config;
        constexpr bool has_config = !std::is_void_v<Config>;
        constexpr CppType config_type = []() noexcept
        {
            if constexpr (has_config)
            {
                return cppType<Config>();
            }
            else
            {
                return CppType{};
            }
        }();
        constexpr auto compatible = []() noexcept -> bool (*)(const void*, const void*) noexcept
        {
            if constexpr (has_config)
            {
                static_assert(std::same_as<decltype(Compatible), bool (*)(const Native&, const Config&) noexcept>);
                return [](const void* instance, const void* config) noexcept
                { return Compatible(*static_cast<const Native*>(instance), *static_cast<const Config*>(config)); };
            }
            else
            {
                static_assert(Compatible == nullptr);
                return nullptr;
            }
        }();
        const void* ops_pointer = nullptr;
        if constexpr (!std::is_void_v<typename Declaration::Ops>)
        {
            ops_pointer = &ops;
        }
        return {
            &Declaration::descriptor,
            ops_pointer,
            [](void* device, StorageId id, const void* config) noexcept -> RenderResult<StorageInstance>
            {
                auto candidate = [&]() noexcept
                {
                    if constexpr (has_config)
                    {
                        static_assert(std::same_as<
                                      decltype(Create),
                                      RenderResult<
                                          std::unique_ptr<Native>> (*)(Device&, StorageId, const Config&) noexcept>);
                        return Create(*static_cast<Device*>(device), id, *static_cast<const Config*>(config));
                    }
                    else
                    {
                        static_assert(std::same_as<
                                      decltype(Create),
                                      RenderResult<std::unique_ptr<Native>> (*)(Device&, StorageId) noexcept>);
                        return Create(*static_cast<Device*>(device), id);
                    }
                }();
                if (!candidate)
                {
                    return cxx::unexpected(candidate.error());
                }
                return StorageInstance{
                    candidate->release(),
                    [](void* value) noexcept { delete static_cast<Native*>(value); }
                };
            },
            compatible,
            binding_ops,
            config_type,
            storageOpsType<Declaration>(),
            cppType<Native>()
        };
    }

    [[nodiscard]] LUX_RENDER_PUBLIC RenderResult<void> validateStorageFactory(
        const StorageDescriptor& type,
        const StorageFactory& factory
    ) noexcept;
} // namespace lux::render
