#pragma once

#include <array>
#include <concepts>
#include <cstdint>
#include <lux/engine/render/extensions/resource/ResourceDefinition.hpp>
#include <span>
#include <tuple>
#include <type_traits>

namespace lux::render
{
    struct CapabilityDescription final
    {
        cxx::basic_type_info type;
        // Discrete operations, with their own Request/Response contracts.
        std::span<const CommandDescriptor* const> commands;
        // Resource identity and input/command contracts are declared once by each Resource.
        std::span<const ResourceDefinition* const> resources;
        std::span<const cxx::basic_type_info> prerequisites;

        constexpr bool operator==(const CapabilityDescription& other) const noexcept
        {
            return type == other.type;
        }
    };

    namespace capability_detail
    {
        template <class T> inline constexpr bool is_tuple = false;
        template <class... T> inline constexpr bool is_tuple<std::tuple<T...>> = true;

        template <class First, class Second> struct JoinedTypes;

        template <class... First, class... Second>
        struct JoinedTypes<std::tuple<First...>, std::tuple<Second...>>
        {
            using Type = std::tuple<First..., Second...>;
        };

        template <class... T> consteval bool uniqueTypes(std::type_identity<std::tuple<T...>>) noexcept
        {
            constexpr std::array types{cxx::make_basic_type_info<void>(), cxx::make_basic_type_info<T>()...};
            for (std::size_t i = 1; i < types.size(); ++i)
            {
                for (std::size_t j = 0; j < i; ++j)
                {
                    if (types[i] == types[j])
                    {
                        return false;
                    }
                }
            }
            return ((std::is_object_v<T> && std::same_as<T, std::remove_cvref_t<T>>) && ...);
        }

        template <RenderCommand... C>
        constexpr auto describeCommands(std::type_identity<std::tuple<C...>>) noexcept
        {
            return std::array<const CommandDescriptor*, sizeof...(C)>{&commandDescriptor<C>()...};
        }

        template <class... C> consteval bool declaredCommands(std::type_identity<std::tuple<C...>>) noexcept
        {
            return (CapabilityCommand<C> && ...);
        }

        template <class... T> constexpr auto describeTypes(std::type_identity<std::tuple<T...>>) noexcept
        {
            return std::array<cxx::basic_type_info, sizeof...(T)>{cxx::make_basic_type_info<T>()...};
        }

        template <class... R> constexpr auto describeResources(std::type_identity<std::tuple<R...>>) noexcept
        {
            return std::array<const ResourceDefinition*, sizeof...(R)>{&R::descriptor...};
        }

        template <class... R> consteval bool declaredResources(std::type_identity<std::tuple<R...>>) noexcept
        {
            return (requires { { R::descriptor } -> std::convertible_to<const ResourceDefinition&>; } && ...);
        }

        template <class... C> consteval bool declaredPrerequisites(std::type_identity<std::tuple<C...>>) noexcept
        {
            return (requires {
                typename C::Commands;
                typename C::Resources;
                typename C::PrerequisiteCapabilities;
            } && ...);
        }

        template <class C> consteval bool validDeclaration() noexcept
        {
            using Commands = typename C::Commands;
            using Resources = typename C::Resources;
            using Prerequisites = typename C::PrerequisiteCapabilities;
            if constexpr (!is_tuple<Commands> || !is_tuple<Resources> || !is_tuple<Prerequisites>)
            {
                return false;
            }
            else
            {
                using Dependencies = typename JoinedTypes<std::tuple<C>, Prerequisites>::Type;
                return uniqueTypes(std::type_identity<Commands>{}) &&
                       uniqueTypes(std::type_identity<Resources>{}) &&
                       declaredResources(std::type_identity<Resources>{}) &&
                       declaredCommands(std::type_identity<Commands>{}) &&
                       uniqueTypes(std::type_identity<Dependencies>{}) &&
                       declaredPrerequisites(std::type_identity<Prerequisites>{});
            }
        }
    } // namespace capability_detail

    template <class C>
    concept CapabilityConcept = requires {
        typename C::Commands;
        typename C::Resources;
        typename C::PrerequisiteCapabilities;
    } && capability_detail::validDeclaration<C>();

    // Static metadata only. No capability object, resource factory or vector is constructed.
    // The returned spans remain valid for the lifetime of the defining application module.
    template <CapabilityConcept C> const CapabilityDescription& describeCapability() noexcept
    {
        static constexpr auto commands =
            capability_detail::describeCommands(std::type_identity<typename C::Commands>{});
        static const auto resources =
            capability_detail::describeResources(std::type_identity<typename C::Resources>{});
        static constexpr auto prerequisites =
            capability_detail::describeTypes(std::type_identity<typename C::PrerequisiteCapabilities>{});
        static const CapabilityDescription description{
            cxx::make_basic_type_info<C>(), commands, resources, prerequisites
        };
        return description;
    }
} // namespace lux::render
