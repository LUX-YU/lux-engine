#pragma once

#include <memory>

#include <lux/cxx/compile_time/type_info.hpp>

#include <concepts>
#include <string_view>

namespace lux::scene
{
    // Named construction input with a qualified C++ contract. A role distinguishes
    // independently supplied uses of the same type; this is not a rendering capability.
    // The caller owns the value and supplies the lifetime required by preparation/installation.
    struct ResolvedSceneDependency final
    {
        std::string_view name;
        std::string_view role;
        lux::cxx::type_info type;
        void* value{};
    };

    template <class Contract, class Concrete>
    [[nodiscard]] ResolvedSceneDependency makeSceneDependency(
        std::string_view name,
        std::string_view role,
        Concrete& value
    ) noexcept
    {
        static_assert(std::same_as<Contract, Concrete> || std::derived_from<Concrete, Contract>);
        return ResolvedSceneDependency{
            name,
            role,
            lux::cxx::make_basic_type_info<Contract>(),
            static_cast<Contract*>(std::addressof(value))
        };
    }
} // namespace lux::scene
