#pragma once

#include <lux/engine/core/visibility.h>

namespace lux::object::detail
{
    struct ObjectState;
    struct SignalStorage;
    struct ConnectionControl;

    LUX_CORE_PUBLIC void intrusive_ptr_add_ref(ObjectState*) noexcept;
    LUX_CORE_PUBLIC void intrusive_ptr_release(ObjectState*) noexcept;
    LUX_CORE_PUBLIC void intrusive_ptr_add_ref(SignalStorage*) noexcept;
    LUX_CORE_PUBLIC void intrusive_ptr_release(SignalStorage*) noexcept;
    LUX_CORE_PUBLIC void intrusive_ptr_add_ref(ConnectionControl*) noexcept;
    LUX_CORE_PUBLIC void intrusive_ptr_release(ConnectionControl*) noexcept;
    LUX_CORE_PUBLIC void closeSignal(SignalStorage*) noexcept;
    LUX_CORE_PUBLIC void invokeConnection(ConnectionControl*, const void*) noexcept;
}
