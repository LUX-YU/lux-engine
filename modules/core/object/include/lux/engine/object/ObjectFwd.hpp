#pragma once

#include <lux/engine/core/visibility.h>

namespace lux::object::detail
{
    struct ObjectState;
    struct ConnectionControl;

    LUX_CORE_PUBLIC void intrusive_ptr_add_ref(ObjectState* state) noexcept;
    LUX_CORE_PUBLIC void intrusive_ptr_release(ObjectState* state) noexcept;
    LUX_CORE_PUBLIC void intrusive_ptr_add_ref(ConnectionControl* control) noexcept;
    LUX_CORE_PUBLIC void intrusive_ptr_release(ConnectionControl* control) noexcept;
} // namespace lux::object::detail
