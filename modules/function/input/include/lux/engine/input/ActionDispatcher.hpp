#pragma once
#include "ActionMapper.hpp"

namespace lux::input
{
    /// Abstract dispatcher interface — useful for decoupling UI/gameplay.
    ///
    /// All dispatch methods use InputValue to preserve the full type and
    /// dimensionality of the action's value (Bool, Axis1D, 2D, 3D).
    class IActionDispatcher
    {
    public:
        virtual ~IActionDispatcher() = default;

        /// Dispatch a one-frame triggered event with the given value.
        virtual void dispatchTriggered(ActionId id, const InputValue& value) = 0;

        /// Dispatch a persistent value (e.g. continuous axis).
        virtual void dispatchValue(ActionId id, const InputValue& value) = 0;
    };

    // -----------------------------------------------------------------------

    /// Concrete dispatcher that forwards injection calls to an ActionMapper.
    class ActionMapperDispatcher final : public IActionDispatcher
    {
    public:
        explicit ActionMapperDispatcher(ActionMapper& mapper) : mapper_(mapper) {}

        void dispatchTriggered(ActionId id, const InputValue& value) override
        { mapper_.injectTriggered(id, value); }

        void dispatchValue(ActionId id, const InputValue& value) override
        { mapper_.injectValue(id, value); }

    private:
        ActionMapper& mapper_;
    };

} // namespace lux::input
