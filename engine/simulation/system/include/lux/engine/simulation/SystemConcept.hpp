#pragma once

#include <lux/engine/simulation/SystemAccessSpec.hpp>
#include <lux/engine/simulation/SystemDescription.hpp>

#include <type_traits>

namespace lux::simulation
{
    /**
     * A System is a concrete type with trusted scheduling metadata and a
     * compile-time-valid durable semantic declaration.
     * It is NOT required to inherit a base class or expose update(Context).
     * The application binds the concrete object into a TaskGraph lambda.
     */
    template <class Type>
    concept System = requires
    {
        requires detail::TrustedSystemAccessDescriptor<decltype(Type::Access)>;
        requires validSystemDescription(Type::Description);
        requires std::is_nothrow_destructible_v<Type>;
    };
}
