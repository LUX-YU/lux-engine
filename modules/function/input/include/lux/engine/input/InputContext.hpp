#pragma once
#include "ActionMap.hpp"
#include <lux/engine/function/visibility.h>
#include <string>

namespace lux::input
{
    namespace detail
    {
        struct InputActivation;
    }

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
    // Fixed-address action data. Destruction revokes all activations without retaining
    // any stack or this semantic object. All access uses the Input owner's thread.
    class LUX_FUNCTION_PUBLIC InputContext final
    {
    public:
        explicit InputContext(std::string name, bool consumes_keyboard = false, bool consumes_mouse = false) noexcept;
        ~InputContext() noexcept;

        InputContext(const InputContext&) = delete;
        InputContext& operator=(const InputContext&) = delete;
        InputContext(InputContext&&) = delete;
        InputContext& operator=(InputContext&&) = delete;

        // ------------------------------------------------------------------ //
        //  Identity                                                           //
        // ------------------------------------------------------------------ //

        [[nodiscard]] const std::string& name() const noexcept
        {
            return name_;
        }

        // ------------------------------------------------------------------ //
        //  ActionMap access                                                   //
        // ------------------------------------------------------------------ //

        [[nodiscard]] ActionMap& actionMap() noexcept
        {
            return action_map_;
        }

        [[nodiscard]] const ActionMap& actionMap() const noexcept
        {
            return action_map_;
        }

        // ------------------------------------------------------------------ //
        //  Consume flags                                                      //
        // ------------------------------------------------------------------ //

        [[nodiscard]] bool consumesKeyboard() const noexcept
        {
            return consumes_keyboard_;
        }

        [[nodiscard]] bool consumesMouse() const noexcept
        {
            return consumes_mouse_;
        }

        void setConsumesKeyboard(bool v) noexcept
        {
            consumes_keyboard_ = v;
        }

        void setConsumesMouse(bool v) noexcept
        {
            consumes_mouse_ = v;
        }

    private:
        friend struct detail::InputActivation;

        std::string name_;
        ActionMap action_map_;
        bool consumes_keyboard_ = false;
        bool consumes_mouse_ = false;
        detail::InputActivation* activation_head_{};
    };

} // namespace lux::input
