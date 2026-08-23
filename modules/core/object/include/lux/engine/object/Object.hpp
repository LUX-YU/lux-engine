#pragma once

#include <concepts>

#include <lux/engine/object/LuxObject.hpp>

namespace lux::object
{
    template <typename Derived, typename Base = LuxObject> class Object : public Base
    {
    public:
        using Base::Base;

        template <typename Payload = void> using signal_type = Signal<Derived, Payload>;

        [[nodiscard]] lux::cxx::TypeToken objectType() const noexcept override
        {
            return lux::cxx::typeToken<Derived>();
        }

        [[nodiscard]] bool isObjectType(lux::cxx::TypeToken type
        ) const noexcept override
        {
            return type == lux::cxx::typeToken<Derived>() || Base::isObjectType(type);
        }
    };
} // namespace lux::object
