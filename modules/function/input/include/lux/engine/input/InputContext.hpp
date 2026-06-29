#pragma once
#include "ActionMap.hpp"
#include <string>

namespace lux::input
{
    /// An InputContext owns an ActionMap and declares whether it consumes
    /// keyboard/mouse events so that lower-priority contexts are suppressed.
    ///
    /// --- Three-layer input blocking semantics ---
    ///
    /// Layer 1 — UI capture (InputSnapshot::keyboard_captured_by_ui / mouse_captured_by_ui):
    ///   The UI system intercepts input before the gameplay input layer sees it.
    ///   When captured, the ActionMapper's want_kb / want_mouse flags should be
    ///   set to false, preventing all gameplay contexts from processing that input.
    ///
    /// Layer 2 — Context-level consume (this class: consumesKeyboard / consumesMouse):
    ///   A high-priority context blocks all *lower-priority* contexts from seeing
    ///   the corresponding input category. This is a coarse, whole-category gate.
    ///   Semantically: "blocks lower-priority contexts for keyboard/mouse".
    ///
    /// Layer 3 — Action-level consume (InputActionDesc::consume_input):
    ///   Fine-grained, per-action consumption within the same evaluation round.
    ///   Reserved for future use — same-priority or same-context action conflicts.
    ///
    class InputContext
    {
    public:
        explicit InputContext(std::string name,
                              bool consumes_keyboard = false,
                              bool consumes_mouse    = false,
                              int  priority          = 0)
            : name_(std::move(name))
            , consumes_keyboard_(consumes_keyboard)
            , consumes_mouse_(consumes_mouse)
            , priority_(priority)
        {}

        // ------------------------------------------------------------------ //
        //  Identity                                                           //
        // ------------------------------------------------------------------ //

        [[nodiscard]] const std::string& name() const noexcept { return name_; }

        // ------------------------------------------------------------------ //
        //  Enabled flag (disabled contexts are skipped during evaluation)     //
        // ------------------------------------------------------------------ //

        [[nodiscard]] bool enabled()        const noexcept { return enabled_;  }
        void               setEnabled(bool v)     noexcept { enabled_ = v;     }

        // ------------------------------------------------------------------ //
        //  Priority (higher value = evaluated first)                          //
        // ------------------------------------------------------------------ //

        [[nodiscard]] int  priority()          const noexcept { return priority_; }
        void               setPriority(int p)        noexcept { priority_ = p;   }

        // ------------------------------------------------------------------ //
        //  ActionMap access                                                   //
        // ------------------------------------------------------------------ //

        [[nodiscard]] ActionMap&       actionMap()       noexcept { return action_map_; }
        [[nodiscard]] const ActionMap& actionMap() const noexcept { return action_map_; }

        // ------------------------------------------------------------------ //
        //  Consume flags                                                      //
        // ------------------------------------------------------------------ //

        [[nodiscard]] bool consumesKeyboard() const noexcept { return consumes_keyboard_; }
        [[nodiscard]] bool consumesMouse()    const noexcept { return consumes_mouse_;    }

        void setConsumesKeyboard(bool v) noexcept { consumes_keyboard_ = v; }
        void setConsumesMouse   (bool v) noexcept { consumes_mouse_    = v; }

    private:
        std::string name_;
        ActionMap   action_map_;
        bool        enabled_           = true;
        bool        consumes_keyboard_ = false;
        bool        consumes_mouse_    = false;
        int         priority_          = 0;
    };

} // namespace lux::input
