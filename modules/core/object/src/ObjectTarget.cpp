#include <lux/engine/object/ObjectTarget.hpp>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/object/detail/ObjectState.hpp>

namespace lux::object
{
    ObjectTarget::ObjectTarget(cxx::intrusive_ptr<detail::ObjectState> state) noexcept : state_(std::move(state)) {}

    ObjectTarget::operator bool() const noexcept
    {
        return static_cast<bool>(state_);
    }

    ObjectTarget LuxObject::target() const noexcept
    {
        assertAffinity();
        return ObjectTarget{ensureState()};
    }

    EObjectPostStatus post(
        const ObjectTarget& target,
        cxx::move_only_function<void(LuxObject*) noexcept> callback,
        std::shared_ptr<const void> code_lifetime
    ) noexcept
    {
        if (!callback)
        {
            detail::failObjectContract();
        }
        if (!target.state_)
        {
            return EObjectPostStatus::CLOSED;
        }
        auto& execution = execution::ExecutionRuntime::instance();
        auto delivery = stdexec::schedule(execution.mainScheduler()) | stdexec::then(
            [state = target.state_, callback = std::move(callback)]() mutable noexcept
            {
                if (execution::ExecutionRuntime::instance().isStopping())
                {
                    callback(nullptr);
                }
                else
                {
                    state->deliver(callback);
                }
            }
        );
        auto accepted = execution.spawn(stdexec::write_env(
            std::move(delivery),
            stdexec::prop{execution::detail::getCompletionCode, code_lifetime}
        ));
        return accepted ? EObjectPostStatus::POSTED : EObjectPostStatus::CLOSED;
    }
} // namespace lux::object
