#pragma once

#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/ecs/RegistryStorageCapacity.hpp>

#include <cstdlib>
#include <string_view>
#include <type_traits>
#include <utility>

namespace lux::ecs
{
    namespace detail
    {
        template <class C>
        [[nodiscard]] bool componentHas(
            RegistryBase& registry,
            Entity entity)
        {
            return registry.all_of<C>(entity);
        }

        template <class C>
        [[nodiscard]] void* componentGet(
            RegistryBase& registry,
            Entity entity)
        {
            if constexpr (std::is_empty_v<C>)
            {
                static C probe{};
                return registry.all_of<C>(entity) ? &probe : nullptr;
            }
            else
                return registry.try_get<C>(entity);
        }

        template <class C>
        [[nodiscard]] void* componentEmplace(
            RegistryBase& registry,
            Entity entity)
        {
            if constexpr (std::is_empty_v<C>)
            {
                registry.emplace_or_replace<C>(entity);
                static C probe{};
                return &probe;
            }
            else
                return &registry.emplace_or_replace<C>(entity);
        }

        template <class C>
        void componentRemove(RegistryBase& registry, Entity entity)
        {
            registry.remove<C>(entity);
        }

        template <class C>
        void componentNotify(RegistryBase& registry, Entity entity)
        {
            if constexpr (!std::is_empty_v<C>)
            {
                if (registry.all_of<C>(entity))
                    registry.patch<C>(entity);
            }
        }

        template <class C>
        [[nodiscard]] void* componentClone(
            RegistryBase& source_registry,
            Entity source,
            RegistryBase& destination_registry,
            Entity destination)
        {
            if constexpr (std::is_empty_v<C>)
            {
                if (!source_registry.all_of<C>(source))
                    return nullptr;
                destination_registry.emplace_or_replace<C>(destination);
                static C probe{};
                return &probe;
            }
            else if constexpr (std::is_copy_constructible_v<C>)
            {
                const auto* value = source_registry.try_get<C>(source);
                return value
                    ? &destination_registry.emplace_or_replace<C>(
                        destination,
                        *value)
                    : nullptr;
            }
            else
                return nullptr;
        }

        template <class C>
        void componentReserve(RegistryBase& registry, std::size_t additional)
        {
            auto& storage = registry.storage<C>();
            if (!reserveAdditionalStorageCapacity(storage, additional))
                std::abort();
        }

        template <class C>
        [[nodiscard]] void* componentTransfer(
            RegistryBase& source_registry,
            Entity source,
            RegistryBase& destination_registry,
            Entity destination) noexcept
        {
            if constexpr (std::is_empty_v<C>)
            {
                if (!source_registry.all_of<C>(source))
                    return nullptr;
                destination_registry.emplace_or_replace<C>(destination);
                source_registry.remove<C>(source);
                static C probe{};
                return &probe;
            }
            else if constexpr (std::is_nothrow_move_constructible_v<C>)
            {
                auto* value = source_registry.try_get<C>(source);
                if (!value)
                    return nullptr;
                auto* result = &destination_registry.emplace_or_replace<C>(
                    destination,
                    std::move(*value)
                );
                source_registry.remove<C>(source);
                return result;
            }
            else
                return nullptr;
        }
    }

    template <class C>
    [[nodiscard]] ComponentSchemaDescriptor makeGeneratedComponentDescriptor(
        const lux::meta::RefClass& ref_class,
        std::string_view schema_name,
        EComponentSerializationPolicy serialization)
    {
        ComponentSchemaDescriptor descriptor;
        descriptor.cpp_type.hash = typeToken<C>().hash;
        descriptor.cpp_type.name = typeToken<C>().name;
        descriptor.schema_id.name = schema_name;
        descriptor.schema_version = 1u;
        descriptor.ref_class = &ref_class;
        descriptor.operations.has = &detail::componentHas<C>;
        descriptor.operations.get = &detail::componentGet<C>;
        descriptor.operations.emplace = &detail::componentEmplace<C>;
        descriptor.operations.remove = &detail::componentRemove<C>;
        descriptor.operations.notify = &detail::componentNotify<C>;
        descriptor.operations.clone = &detail::componentClone<C>;
        descriptor.operations.reserve = &detail::componentReserve<C>;
        descriptor.operations.transfer = &detail::componentTransfer<C>;
        descriptor.operations.no_throw_transfer =
            std::is_empty_v<C> || std::is_nothrow_move_constructible_v<C>;
        descriptor.serialization = serialization;
        descriptor.provider = "org.lux.builtin";
        return descriptor;
    }
}
