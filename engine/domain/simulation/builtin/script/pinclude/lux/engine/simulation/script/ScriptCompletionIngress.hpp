#pragma once

#include <lux/engine/simulation/ScriptSystem.hpp>
#include <lux/engine/simulation/script/ExternalCompletionRing.hpp>

namespace lux::simulation::script::detail
{
    class ScriptCompletionIngress final
    {
        friend struct ScriptCompletionIngressTestAccess;
        struct Transport final
        {
            ExternalCompletionRing completions;

            [[nodiscard]] lux::cxx::expected<void, EScriptAwaitableCompletionError> complete(
                ScriptInstanceId instance,
                ScriptAwaitableId awaitable,
                EScriptAwaitableState state,
                ScriptOwnedResumeValue value,
                ScriptStepError error) noexcept
            {
                ExternalCompletionRecord record{instance, awaitable, state, error};
                if (value.bytes.size() > record.bytes.size())
                    return lux::cxx::unexpected(EScriptAwaitableCompletionError::INVALID_VALUE);
                if (value.type.valid())
                {
                    record.type = value.type.type_id;
                    record.size = static_cast<std::uint32_t>(value.bytes.size());
                }
                if (!value.bytes.empty())
                    std::memcpy(record.bytes.data(), value.bytes.data(), value.bytes.size());
                return completions.push(record);
            }

            [[nodiscard]] static lux::cxx::expected<void, EScriptAwaitableCompletionError> completeErased(
                void* context,
                ScriptInstanceId instance,
                ScriptAwaitableId awaitable,
                EScriptAwaitableState state,
                ScriptOwnedResumeValue value,
                ScriptStepError error) noexcept
            {
                return static_cast<Transport*>(context)->complete(instance,
                                                                         awaitable,
                                                                         state,
                                                                         std::move(value),
                                                                         error);
            }

            [[nodiscard]] static ScriptInstanceId unpackInstance(std::uint64_t value) noexcept
            {
                return {
                    static_cast<std::uint32_t>(value >> 32U),
                    static_cast<std::uint32_t>(value)
                };
            }

            [[nodiscard]] static ScriptAwaitableId unpackAwaitable(std::uint64_t value) noexcept
            {
                return {
                    static_cast<std::uint32_t>(value >> 32U),
                    static_cast<std::uint32_t>(value)
                };
            }

            [[nodiscard]] static lux::script::EScriptAbilityCompletionError abilityError(
                EScriptAwaitableCompletionError error
            ) noexcept
            {
                switch (error)
                {
                case EScriptAwaitableCompletionError::INVALID_ID:
                    return lux::script::EScriptAbilityCompletionError::STALE;
                case EScriptAwaitableCompletionError::INVALID_VALUE:
                    return lux::script::EScriptAbilityCompletionError::INVALID_VALUE;
                case EScriptAwaitableCompletionError::ALREADY_TERMINAL:
                    return lux::script::EScriptAbilityCompletionError::ALREADY_COMPLETED;
                case EScriptAwaitableCompletionError::RESUME_QUEUE_FULL:
                    return lux::script::EScriptAbilityCompletionError::BACKPRESSURE;
                case EScriptAwaitableCompletionError::STOPPING:
                    return lux::script::EScriptAbilityCompletionError::STOPPING;
                }
                return lux::script::EScriptAbilityCompletionError::STALE;
            }

            [[nodiscard]] lux::cxx::expected<void, lux::script::EScriptAbilityCompletionError> completeAbility(
                ScriptInstanceId instance,
                ScriptAwaitableId awaitable,
                lux::semantic::TypeId type,
                const void* data,
                std::uint32_t size
            ) noexcept
            {
                ExternalCompletionRecord record{
                    instance,
                    awaitable,
                    EScriptAwaitableState::READY,
                    {},
                    type,
                    size
                };
                if (size > record.bytes.size() || (size != 0U && data == nullptr))
                    return lux::cxx::unexpected(lux::script::EScriptAbilityCompletionError::INVALID_VALUE);
                if (size != 0U)
                    std::memcpy(record.bytes.data(), data, size);
                const auto completed = completions.push(record);
                return completed
                    ? lux::cxx::expected<void, lux::script::EScriptAbilityCompletionError>{}
                    : lux::cxx::unexpected(abilityError(completed.error()));
            }

            [[nodiscard]] static lux::cxx::expected<void, lux::script::EScriptAbilityCompletionError>
            completeAbilityErased(
                void* context,
                std::uint64_t instance,
                std::uint64_t awaitable,
                lux::semantic::TypeId type,
                const void* data,
                std::uint32_t size
            ) noexcept
            {
                return static_cast<Transport*>(context)->completeAbility(
                    unpackInstance(instance),
                    unpackAwaitable(awaitable),
                    type,
                    data,
                    size
                );
            }

            [[nodiscard]] static lux::cxx::expected<void, lux::script::EScriptAbilityCompletionError>
            failAbilityErased(
                void* context,
                std::uint64_t instance,
                std::uint64_t awaitable,
                lux::script::ScriptAbilityOperationError error
            ) noexcept
            {
                const auto completed = static_cast<Transport*>(context)->complete(
                    unpackInstance(instance),
                    unpackAwaitable(awaitable),
                    EScriptAwaitableState::FAILED,
                    {},
                    {error.status}
                );
                return completed
                    ? lux::cxx::expected<void, lux::script::EScriptAbilityCompletionError>{}
                    : lux::cxx::unexpected(abilityError(completed.error()));
            }

            [[nodiscard]] static bool activeAbilityErased(
                void* context,
                std::uint64_t instance,
                std::uint64_t awaitable
            ) noexcept
            {
                return static_cast<Transport*>(context)->active(
                    unpackInstance(instance),
                    unpackAwaitable(awaitable)
                );
            }

            [[nodiscard]] bool active(ScriptInstanceId instance, ScriptAwaitableId awaitable) noexcept
            {
                static_cast<void>(instance);
                return completions.active(awaitable);
            }

            [[nodiscard]] static bool activeErased(
                void* context,
                ScriptInstanceId instance,
                ScriptAwaitableId awaitable
            ) noexcept
            {
                return static_cast<Transport*>(context)->active(instance, awaitable);
            }
        };

    public:
        // Fallible preparation only; transport outlives this owner through completion leases.
        void prepare(std::size_t capacity, std::size_t physical_awaitables);

        void open(ScriptAwaitableId id, const std::optional<PreparedResumeType>& type) noexcept
        {
            transport_->completions.open(id, type);
        }
        void close(ScriptAwaitableId id) noexcept { transport_->completions.close(id); }
        void stop() noexcept { transport_->completions.stop(); }

        [[nodiscard]] ScriptAwaitableRegistration registration(ScriptInstanceId instance, ScriptAwaitableId id,
            void* owner, ScriptAwaitableCompletion::AbilitySuccessFn success,
            ScriptAwaitableCompletion::AbilityFailureFn fail) noexcept
        {
            ++capability_constructions_;
            return {id, ScriptAwaitableCompletion{std::static_pointer_cast<void>(transport_), transport_.get(),
                &Transport::completeErased, &Transport::activeErased, instance, id,
                &Transport::completeAbilityErased, &Transport::failAbilityErased, &Transport::activeAbilityErased,
                owner, success, fail}};
        }

        void capture() noexcept
        {
            frontier_ = transport_->completions.enqueue_position.load(std::memory_order_acquire);
            remaining_ = transport_->completions.capacity;
            pending_in_window_ = transport_->completions.dequeue_position < frontier_ && remaining_ != 0U;
            prepared_ = true;
        }
        void beginDrain() noexcept
        {
            if (!prepared_)
                capture();
            prepared_ = false;
        }

        // Owner-only const borrow ends at ack(). An unpublished reserved head is never skipped.
        // The attempt consumes the captured window's allowance even if owner admission is backpressured.
        [[nodiscard]] const ExternalCompletionRecord* peek() noexcept
        {
            if (!pending_in_window_)
                return nullptr;
            const auto* result = transport_->completions.front();
            if (result != nullptr && --remaining_ == 0U)
                pending_in_window_ = false;
            // A reserved but unpublished head remains eligible for a later retry.
            return result;
        }
        void ack() noexcept
        {
            transport_->completions.pop();
            if (transport_->completions.dequeue_position >= frontier_)
                pending_in_window_ = false;
        }
        [[nodiscard]] bool hasPendingInWindow() const noexcept { return pending_in_window_; }

        [[nodiscard]] static ScriptInstanceId unpackInstance(std::uint64_t value) noexcept
        {
            return Transport::unpackInstance(value);
        }
        [[nodiscard]] static ScriptAwaitableId unpackAwaitable(std::uint64_t value) noexcept
        {
            return Transport::unpackAwaitable(value);
        }
        [[nodiscard]] static lux::script::EScriptAbilityCompletionError abilityError(
            EScriptAwaitableCompletionError error) noexcept
        {
            return Transport::abilityError(error);
        }

        void writeStats(ScriptRuntimeStats& result) const noexcept;

    private:
        std::shared_ptr<Transport> transport_;
        std::size_t frontier_{};
        std::size_t remaining_{};
        bool prepared_{};
        bool pending_in_window_{};
        std::uint64_t capability_constructions_{};
    };
}
