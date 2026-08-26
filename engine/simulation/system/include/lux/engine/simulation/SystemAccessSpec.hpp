#pragma once

#include <lux/cxx/compile_time/TypeToken.hpp>

#include <entt/core/type_info.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <tuple>
#include <type_traits>

namespace lux::simulation
{
    enum class ESystemAccessMode : std::uint8_t
    {
        READ,
        WRITE,
    };

    struct SystemComponentAccess final
    {
        lux::cxx::TypeToken type;
        std::uint64_t storage{};
        ESystemAccessMode mode{ESystemAccessMode::READ};
    };

    struct SystemExternalAccess final
    {
        lux::cxx::TypeToken type;
        ESystemAccessMode mode{ESystemAccessMode::READ};
    };

    template <class Component>
    struct ComponentRead final
    {
        using component_type = Component;
    };

    template <class Component>
    struct ComponentWrite final
    {
        using component_type = Component;
    };

    template <class External>
    struct ExternalRead final
    {
        using external_type = External;
    };

    template <class External>
    struct ExternalWrite final
    {
        using external_type = External;
    };

    struct SystemAccessSpec final
    {
        std::span<const SystemComponentAccess> components;
        std::span<const SystemExternalAccess> external;
    };

    namespace detail
    {
        template <class Access>
        struct ComponentSystemAccessTraits;

        template <class Component>
        struct ComponentSystemAccessTraits<ComponentRead<Component>> final
        {
            using ComponentType = Component;
            static constexpr ESystemAccessMode kMode = ESystemAccessMode::READ;
        };

        template <class Component>
        struct ComponentSystemAccessTraits<ComponentWrite<Component>> final
        {
            using ComponentType = Component;
            static constexpr ESystemAccessMode kMode = ESystemAccessMode::WRITE;
        };

        template <class Access>
        concept ComponentSystemAccess = requires
        {
            typename ComponentSystemAccessTraits<Access>::ComponentType;
        };

        template <class Access>
        struct ExternalSystemAccessTraits;

        template <class External>
        struct ExternalSystemAccessTraits<ExternalRead<External>> final
        {
            using ExternalType = External;
            static constexpr ESystemAccessMode kMode = ESystemAccessMode::READ;
        };

        template <class External>
        struct ExternalSystemAccessTraits<ExternalWrite<External>> final
        {
            using ExternalType = External;
            static constexpr ESystemAccessMode kMode = ESystemAccessMode::WRITE;
        };

        template <class Access>
        concept ExternalSystemAccess = requires
        {
            typename ExternalSystemAccessTraits<Access>::ExternalType;
        };

        template <class Access>
        inline constexpr bool kSystemAccessElement =
            ComponentSystemAccess<Access> ||
            ExternalSystemAccess<Access>;

        template <class... Access>
        inline constexpr std::size_t kComponentAccessCount =
            (std::size_t{ComponentSystemAccess<Access>} + ... + 0U);

        template <class... Access>
        inline constexpr std::size_t kExternalAccessCount =
            (std::size_t{ExternalSystemAccess<Access>} + ... + 0U);

        template <class... Access>
        [[nodiscard]] consteval auto systemComponentAccesses() noexcept
        {
            std::array<SystemComponentAccess, kComponentAccessCount<Access...>>
                result{};
            std::size_t index{};
            ([&]
            {
                if constexpr (ComponentSystemAccess<Access>)
                {
                    using Component =
                        typename ComponentSystemAccessTraits<Access>::ComponentType;
                    result[index++] = SystemComponentAccess{
                        lux::cxx::typeToken<Component>(),
                        entt::type_hash<Component>::value(),
                        ComponentSystemAccessTraits<Access>::kMode
                    };
                }
            }(), ...);
            return result;
        }

        template <class... Access>
        [[nodiscard]] consteval auto systemExternalAccesses() noexcept
        {
            std::array<SystemExternalAccess, kExternalAccessCount<Access...>>
                result{};
            std::size_t index{};
            ([&]
            {
                if constexpr (ExternalSystemAccess<Access>)
                {
                    using External = typename
                        ExternalSystemAccessTraits<Access>::ExternalType;
                    result[index++] = SystemExternalAccess{
                        lux::cxx::typeToken<External>(),
                        ExternalSystemAccessTraits<Access>::kMode
                    };
                }
            }(), ...);
            return result;
        }

        template <class Value, std::size_t Size>
        [[nodiscard]] consteval bool uniqueSystemAccesses(
            const std::array<Value, Size>& values
        ) noexcept
        {
            for (std::size_t current{}; current < values.size(); ++current)
            {
                for (std::size_t previous{}; previous < current; ++previous)
                {
                    if (values[current].type == values[previous].type)
                        return false;
                }
            }
            return true;
        }
    }

    template <class... Access>
        requires (detail::kSystemAccessElement<Access> && ...)
    class StaticSystemAccessDescriptor final
    {
      private:
        inline static constexpr auto kComponents =
            detail::systemComponentAccesses<Access...>();
        inline static constexpr auto kExternal =
            detail::systemExternalAccesses<Access...>();

      public:
        static_assert(detail::uniqueSystemAccesses(kComponents));
        static_assert(detail::uniqueSystemAccesses(kExternal));

        [[nodiscard]] constexpr SystemAccessSpec spec() const noexcept
        {
            return SystemAccessSpec{kComponents, kExternal};
        }
    };

    namespace detail
    {
        template <class Type>
        inline constexpr bool kTrustedSystemAccessDescriptor = false;

        template <class... Access>
        inline constexpr bool kTrustedSystemAccessDescriptor<
            StaticSystemAccessDescriptor<Access...>> = true;

        template <class Type>
        concept TrustedSystemAccessDescriptor =
            kTrustedSystemAccessDescriptor<std::remove_cvref_t<Type>>;
    }

    template <class... Access>
        requires (detail::kSystemAccessElement<Access> && ...)
    [[nodiscard]] consteval auto makeSystemAccessSpec() noexcept
    {
        return StaticSystemAccessDescriptor<Access...>{};
    }

}
