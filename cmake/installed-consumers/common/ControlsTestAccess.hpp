#pragma once
#include <lux/engine/ui/Controls.hpp>

namespace lux::ui
{
    // Tests inject an already interpreted user gesture at the signal boundary.
    // Programmatic setValue still does not emit and production emit remains protected.
    struct ControlsTestAccess final
    {
        template <class Control> static object::SignalDelivery edited(Control& control, EditResult value) noexcept
        {
            return control.emit(control.edited, value);
        }
        static object::SignalDelivery activate(Button& control) noexcept
        {
            return control.emit(control.activated);
        }
    };
}
