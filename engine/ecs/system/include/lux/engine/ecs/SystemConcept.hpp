#pragma once

#include <lux/engine/ecs/SystemAccessSpec.hpp>

namespace lux::ecs
{
    /**
     * A System is only a type with trusted scheduling metadata.
     * It is NOT required to inherit a base class or expose update(Context).
     * The application binds the concrete object into a TaskGraph lambda.
     */
    template <class Type>
    concept System = requires
    {
        requires detail::TrustedSystemAccessDescriptor<decltype(Type::Access)>;
    };
}
