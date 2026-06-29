#pragma once
#include <lux/engine/window/LuxWindowDefination.hpp>
#include <variant>

namespace lux::input
{
    // -------------------------------------------------------------------------
    // Physical input sources
    // -------------------------------------------------------------------------

    struct KeyInput
    {
        lux::window::KeyEnum key;
    };

    struct MouseButtonInput
    {
        lux::window::MouseButton button;
    };

    struct MouseAxisInput
    {
        enum class EAxis : uint8_t
        {
            DELTA_X,   ///< Horizontal cursor movement (positive = right)
            DELTA_Y,   ///< Vertical cursor movement   (positive = down)
            SCROLL_X,  ///< Horizontal scroll           (positive = right)
            SCROLL_Y,  ///< Mouse wheel                (positive = up / toward user)
        } axis;

        /// Device-layer raw axis scaling. Applied before any binding-level modifiers.
        /// For user-facing sensitivity, prefer EModifierKind::SCALE at the binding level.
        float scale = 1.0f;
    };

    // -------------------------------------------------------------------------
    // PhysicalInput — currently keyboard/mouse only.
    //
    // TODO: Migrate to a device-agnostic RawControlPath for gamepad, rebinding,
    //       device slot, and local multiplayer support:
    //   struct RawControlPath {
    //       DeviceType   device_type;
    //       uint16_t     device_slot;
    //       uint16_t     control_code;
    //       RawControlKind control_kind;
    //   };
    // -------------------------------------------------------------------------
    using PhysicalInput = std::variant<KeyInput, MouseButtonInput, MouseAxisInput>;

} // namespace lux::input
