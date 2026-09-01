#pragma once

#include <lux/engine/object/LuxObject.hpp>

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
        object::LuxObject* object{};
    };

    template <class Contract, class Concrete>
    [[nodiscard]] SceneCapabilityProvider makeSceneCapabilityProvider(
        std::string_view name,
        std::string_view capability,
        Concrete& value
    ) noexcept
    {
        static_assert(std::same_as<Contract, Concrete> || std::derived_from<Concrete, Contract>);
        object::LuxObject* object_ptr = nullptr;
        if constexpr (std::derived_from<Concrete, object::LuxObject>)
        {
            object_ptr = static_cast<object::LuxObject*>(std::addressof(value));
        }
        return SceneCapabilityProvider{
            name,
            capability,
            lux::cxx::typeToken<Contract>(),
            static_cast<Contract*>(std::addressof(value)),
            object_ptr
        };
    }
} // namespace lux::scene
