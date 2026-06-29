#pragma once
#include "InputBindingState.hpp"  // for BindingId
#include <lux/engine/function/visibility.h>

namespace lux::input
{
    class BindingIdAllocator
    {
    public:
        LUX_FUNCTION_PUBLIC static BindingId next() noexcept;
    };

} // namespace lux::input
