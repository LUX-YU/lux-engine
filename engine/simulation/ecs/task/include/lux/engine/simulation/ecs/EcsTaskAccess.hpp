#pragma once

#include <lux/engine/simulation/ecs/EcsTaskResource.hpp>
#include <lux/engine/simulation/ecs/Query.hpp>
#include <lux/engine/simulation/SystemAccessSpec.hpp>
#include <lux/engine/simulation/ecs/EcsTaskResources.hpp>
#include <lux/engine/task/TaskGraph.hpp>

#include <entt/core/type_info.hpp>

#include <array>
#include <concepts>
#include <cstdint>
#include <span>
#include <type_traits>

namespace lux::simulation::ecs
{
    namespace detail
    {
        template <class Access>
        struct EcsTaskAccessTraits;

        template <class Component>
        struct EcsTaskAccessTraits<Read<Component>> final
        {
            static constexpr bool kComponent = true;
            static constexpr bool kWrite = false;
            [[nodiscard]] static constexpr task::TaskResourceKey key() noexcept
            {
                return componentTaskResource(
                    entt::type_hash<Component>::value()
                );
            }
        };

        template <class Component>
        struct EcsTaskAccessTraits<Write<Component>> final
        {
            static constexpr bool kComponent = true;
            static constexpr bool kWrite = true;
            [[nodiscard]] static constexpr task::TaskResourceKey key() noexcept
            {
                return componentTaskResource(
                    entt::type_hash<Component>::value()
                );
            }
        };

        template <class Resource>
    struct EcsTaskAccessTraits<ExternalRead<Resource>> final
    {
        using ResourceType = Resource;
            static constexpr bool kComponent = false;
            static constexpr bool kWrite = false;
            [[nodiscard]] static constexpr task::TaskResourceKey key() noexcept
            {
                return externalTaskResource<Resource>();
            }
        };

        template <class Resource>
    struct EcsTaskAccessTraits<ExternalWrite<Resource>> final
    {
        using ResourceType = Resource;
            static constexpr bool kComponent = false;
            static constexpr bool kWrite = true;
            [[nodiscard]] static constexpr task::TaskResourceKey key() noexcept
            {
                return externalTaskResource<Resource>();
            }
        };

        template <class Access>
        concept EcsTaskAccessValue = requires
        {
            { EcsTaskAccessTraits<Access>::key() } ->
                std::same_as<task::TaskResourceKey>;
        };

        template <class Value, class Spec>
        struct PrependQuery;

        template <class Value, class... Tail>
        struct PrependQuery<Value, QuerySpec<Tail...>> final
        {
            using Type = QuerySpec<Value, Tail...>;
        };

        template <class... Access>
        struct ComponentQuery;

        template <>
        struct ComponentQuery<> final
        {
            using Type = QuerySpec<>;
        };

        template <class First, class... Rest>
        struct ComponentQuery<First, Rest...> final
        {
            using Tail = typename ComponentQuery<Rest...>::Type;
            using Type = std::conditional_t<
                EcsTaskAccessTraits<First>::kComponent,
                typename PrependQuery<First, Tail>::Type,
                Tail>;
        };

        template <class Component, class... Access>
        inline constexpr bool kDeclaresWrite =
            ((std::same_as<Access, Write<Component>>) || ...);
    }

    template <detail::EcsTaskAccessValue... Access>
    class EcsTaskAccess final
    {
      public:
        constexpr EcsTaskAccess() noexcept = default;

        [[nodiscard]] constexpr std::span<const task::TaskResourceAccess>
        resources() const noexcept
        {
            return values_;
        }

        [[nodiscard]] constexpr std::span<const std::uint64_t>
        writeStorages() const noexcept
        {
            return write_storages_;
        }

        [[nodiscard]] constexpr operator std::span<
            const task::TaskResourceAccess>() const noexcept
        {
            return values_;
        }

      private:
        static constexpr auto makeResource(auto tag) noexcept
        {
            using Value = typename decltype(tag)::type;
            constexpr auto key = detail::EcsTaskAccessTraits<Value>::key();
            if constexpr (detail::EcsTaskAccessTraits<Value>::kWrite)
                return task::write(key);
            else
                return task::read(key);
        }

        std::array<task::TaskResourceAccess, sizeof...(Access)> values_{
            makeResource(std::type_identity<Access>{})...
        };
        static constexpr std::size_t kWriteComponentCount =
            (std::size_t{} + ... +
             (detail::EcsTaskAccessTraits<Access>::kComponent &&
              detail::EcsTaskAccessTraits<Access>::kWrite ? 1U : 0U));

        static consteval auto makeWriteStorages() noexcept
        {
            std::array<std::uint64_t, kWriteComponentCount> result{};
            std::size_t index{};
            ([&]
            {
                if constexpr (
                    detail::EcsTaskAccessTraits<Access>::kComponent &&
                    detail::EcsTaskAccessTraits<Access>::kWrite)
                {
                    result[index++] =
                        detail::EcsTaskAccessTraits<Access>::key().value;
                }
            }(), ...);
            return result;
        }

        std::array<std::uint64_t, kWriteComponentCount> write_storages_{
            makeWriteStorages()
        };

        template <class... Value>
        friend auto taskQuery(
            EcsState&,
            EcsChangeBatch&,
            EcsTaskAccess<Value...>
        );
        template <class Component, class... Value>
        friend TaskWriter<Component> taskWriter(
            EcsState&,
            EcsChangeBatch&,
            EcsTaskAccess<Value...>
        ) noexcept;
    };

    template <class... Access>
    inline constexpr EcsTaskAccess<Access...> access{};

    template <class... Access>
    [[nodiscard]] auto taskQuery(
        EcsState& state,
        EcsChangeBatch& changes,
        EcsTaskAccess<Access...>
    )
    {
        using Spec = typename detail::ComponentQuery<Access...>::Type;
        static_assert(!std::same_as<Spec, QuerySpec<>>);
        return lux::simulation::ecs::taskQuery(state, changes, Spec{});
    }

    template <class Component, class... Access>
    [[nodiscard]] TaskWriter<Component> taskWriter(
        EcsState& state,
        EcsChangeBatch& changes,
        EcsTaskAccess<Access...>
    ) noexcept
    {
        static_assert(detail::kDeclaresWrite<Component, Access...>);
        return lux::simulation::ecs::taskWriter<Component>(state, changes);
    }

    [[nodiscard]] constexpr task::TaskResourceAccess ecsStructureWrite()
        noexcept
    {
        return task::write(ecsStructureTaskResource());
    }

    [[nodiscard]] constexpr task::TaskResourceAccess ecsChangesWrite()
        noexcept
    {
        return task::write(ecsChangesTaskResource());
    }

    [[nodiscard]] constexpr task::TaskResourceAccess ecsChangesRead()
        noexcept
    {
        return task::read(ecsChangesTaskResource());
    }

    [[nodiscard]] constexpr task::TaskResourceAccess ecsCommandsWrite()
        noexcept
    {
        return task::write(ecsCommandsTaskResource());
    }
}
