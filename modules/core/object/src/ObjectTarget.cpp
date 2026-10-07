#include <lux/engine/object/ObjectTarget.hpp>
#include <lux/engine/object/detail/MessageEnvelope.hpp>
#include <lux/engine/object/detail/ObjectState.hpp>

namespace lux::object
{
    namespace
    {
        struct TargetMessage final
        {
            cxx::intrusive_ptr<detail::ObjectState> state;
            cxx::move_only_function<void(LuxObject*) noexcept> callback;

            void operator()() noexcept
            {
                state->deliver(callback);
            }
            void cancel() noexcept
            {
                // Runtime shutdown never invokes target business code; completion must still settle.
                callback(nullptr);
            }
        };
    } // namespace

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
        cxx::move_only_function<void(LuxObject*) noexcept> callback
    ) noexcept
    {
        if (!target.state_)
        {
            return EObjectPostStatus::CLOSED;
        }
        if (!callback)
        {
            detail::failObjectContract();
        }
        auto message = detail::makeCompletionMessage(TargetMessage{target.state_, std::move(callback)});
        switch (detail::post(std::move(message)))
        {
        case detail::EPostStatus::POSTED:
            return EObjectPostStatus::POSTED;
        case detail::EPostStatus::CLOSED:
            return EObjectPostStatus::CLOSED;
        case detail::EPostStatus::FULL:
            return EObjectPostStatus::FULL;
        }
        detail::failObjectContract();
    }
} // namespace lux::object
