#pragma once

#include <memory>

#include <lux/cxx/compile_time/TypeToken.hpp>

#include <concepts>
#include <string_view>

namespace lux::scene
{
    struct SceneCapabilityProvider final
    {
        std::string_view name;
        std::string_view capability;
        lux::cxx::TypeToken type;
        void* value{};
    };

    template <class Contract, class Concrete>
    [[nodiscard]] SceneCapabilityProvider makeSceneCapabilityProvider(
        std::string_view name,
        std::string_view capability,
        Concrete& value
    ) noexcept
    {
        static_assert(std::same_as<Contract, Concrete> || std::derived_from<Concrete, Contract>);
        return SceneCapabilityProvider{
            name,
            capability,
            lux::cxx::typeToken<Contract>(),
            static_cast<Contract*>(std::addressof(value))
        };
    }
} // namespace lux::scene
