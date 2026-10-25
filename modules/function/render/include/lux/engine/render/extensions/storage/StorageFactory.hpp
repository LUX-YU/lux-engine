#pragma once

#include <lux/engine/render/visibility.h>

#include <concepts>
#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/engine/render/extensions/storage/StorageCreateContext.hpp>
#include <lux/engine/render/extensions/storage/StorageDescriptor.hpp>
#include <memory>

namespace lux::render
{
    // One owned native instance. The factory's matching Ops are separate immutable metadata.
    using StorageInstance = std::unique_ptr<void, void (*)(void*) noexcept>;

    template <class Declaration> [[nodiscard]] constexpr cxx::TypeToken storageOpsType() noexcept
    {
        if constexpr (std::is_void_v<typename Declaration::Ops>)
        {
            return {};
        }
        else
        {
            return cxx::typeToken<typename Declaration::Ops>();
        }
    }

    template <class Ops>
    using StorageOpsArgument =
        std::conditional_t<std::is_void_v<Ops>, std::nullptr_t, std::add_lvalue_reference_t<std::add_const_t<Ops>>>;

    struct StorageFactory final
    {
        const StorageDescriptor* const descriptor;
        const void* const ops;
        RenderResult<StorageInstance> (*const create)(void* dependencies, StorageId id, const void* config) noexcept;
        bool (*const compatible_config)(const void* instance, const void* config) noexcept;
        const void* const binding_ops; // API-specific immutable preparation functions; never a second owner
        const cxx::TypeToken config_type;
        const cxx::TypeToken ops_type;
        const cxx::TypeToken implementation_type;

    private:
        template <class Declaration, class Implementation, class CreationContext, auto Create, auto Compatible>
        friend StorageFactory makeStorageFactory(StorageOpsArgument<typename Declaration::Ops>, const void*) noexcept;

        StorageFactory(
            const StorageDescriptor* descriptor,
            const void* ops,
            RenderResult<StorageInstance> (*create)(void*, StorageId, const void*) noexcept,
            bool (*compatible_config)(const void*, const void*) noexcept,
            const void* binding_ops,
            cxx::TypeToken config_type,
            cxx::TypeToken ops_type,
            cxx::TypeToken implementation_type
        ) noexcept
            : descriptor(descriptor), ops(ops), create(create), compatible_config(compatible_config),
              binding_ops(binding_ops), config_type(config_type), ops_type(ops_type),
              implementation_type(implementation_type)
        {
        }
    };

    // Create and compatible callbacks are typed before the only erasure boundary.
    template <class Declaration, class Implementation, class CreationContext, auto Create, auto Compatible = nullptr>
    [[nodiscard]] StorageFactory makeStorageFactory(
        StorageOpsArgument<typename Declaration::Ops> ops,
        const void* binding_ops = nullptr
    ) noexcept
    {
        using Config = typename Declaration::Config;
        constexpr bool has_config = !std::is_void_v<Config>;
        constexpr cxx::TypeToken config_type = []() noexcept
        {
            if constexpr (has_config)
            {
                return cxx::typeToken<Config>();
            }
            else
            {
                return cxx::TypeToken{};
            }
        }();
        constexpr auto compatible = []() noexcept -> bool (*)(const void*, const void*) noexcept
        {
            if constexpr (has_config)
            {
                static_assert(std::same_as<
                              decltype(Compatible),
                              bool (*)(const Implementation&, const Config&) noexcept>);
                return [](const void* instance, const void* config) noexcept
                {
                    return Compatible(
                        *static_cast<const Implementation*>(instance),
                        *static_cast<const Config*>(config)
                    );
                };
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
            [](void* dependencies, StorageId id, const void* config) noexcept -> RenderResult<StorageInstance>
            {
                auto candidate = [&]() noexcept
                {
                    if constexpr (has_config)
                    {
                        static_assert(std::same_as<
                                      decltype(Create),
                                      RenderResult<std::unique_ptr<
                                          Implementation>> (*)(CreationContext&, StorageId, const Config&) noexcept>);
                        return Create(
                            *static_cast<CreationContext*>(dependencies),
                            id,
                            *static_cast<const Config*>(config)
                        );
                    }
                    else
                    {
                        static_assert(std::same_as<
                                      decltype(Create),
                                      RenderResult<
                                          std::unique_ptr<Implementation>> (*)(CreationContext&, StorageId) noexcept>);
                        return Create(*static_cast<CreationContext*>(dependencies), id);
                    }
                }();
                if (!candidate)
                {
                    return cxx::unexpected(candidate.error());
                }
                return StorageInstance{
                    candidate->release(),
                    [](void* value) noexcept { delete static_cast<Implementation*>(value); }
                };
            },
            compatible,
            binding_ops,
            config_type,
            storageOpsType<Declaration>(),
            cxx::typeToken<Implementation>()
        };
    }

    [[nodiscard]] LUX_RENDER_PUBLIC RenderResult<void> validateStorageFactory(
        const StorageDescriptor& type,
        const StorageFactory& factory
    ) noexcept;
} // namespace lux::render
