#pragma once

#include <lux/engine/description/Script.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace lux::simulation::script
{
    struct ScriptInstanceId final
    {
        std::uint32_t slot{};
        std::uint32_t generation{};

        [[nodiscard]] constexpr bool valid() const noexcept
        {
            return slot != 0U && generation != 0U;
        }

        friend constexpr bool operator==(ScriptInstanceId, ScriptInstanceId) noexcept = default;
    };

    struct ScriptContinuationId final
    {
        std::uint32_t slot{};
        std::uint32_t generation{};

        [[nodiscard]] constexpr bool valid() const noexcept
        {
            return slot != 0U && generation != 0U;
        }

        friend constexpr bool operator==(ScriptContinuationId, ScriptContinuationId) noexcept = default;
    };

    struct ScriptAwaitableId final
    {
        std::uint32_t slot{};
        std::uint32_t generation{};

        [[nodiscard]] constexpr bool valid() const noexcept
        {
            return slot != 0U && generation != 0U;
        }

        friend constexpr bool operator==(ScriptAwaitableId, ScriptAwaitableId) noexcept = default;
    };

    enum class EScriptStepState : std::uint8_t
    {
        COMPLETED,
        SUSPENDED,
        FAILED,
    };

    enum class EScriptAwaitableState : std::uint8_t
    {
        PENDING,
        READY,
        CANCELLED,
        FAILED,
    };

    struct ScriptStepError final
    {
        std::int32_t status{};

        [[nodiscard]] constexpr bool valid() const noexcept
        {
            return status != 0;
        }

        friend constexpr bool operator==(ScriptStepError, ScriptStepError) noexcept = default;
    };

    struct ScriptStepResult final
    {
        EScriptStepState state{EScriptStepState::COMPLETED};
        ScriptAwaitableId waiting_on;
        ScriptStepError error;

        [[nodiscard]] static constexpr ScriptStepResult completed() noexcept
        {
            return {};
        }

        [[nodiscard]] static constexpr ScriptStepResult suspended(ScriptAwaitableId waiting_on) noexcept
        {
            return {EScriptStepState::SUSPENDED, waiting_on, {}};
        }

        [[nodiscard]] static constexpr ScriptStepResult failed(std::int32_t status) noexcept
        {
            return {EScriptStepState::FAILED, {}, {status}};
        }

        [[nodiscard]] constexpr bool valid() const noexcept
        {
            switch (state)
            {
            case EScriptStepState::COMPLETED:
                return !waiting_on.valid() && !error.valid();
            case EScriptStepState::SUSPENDED:
                return waiting_on.valid() && !error.valid();
            case EScriptStepState::FAILED:
                return !waiting_on.valid() && error.valid();
            }
            return false;
        }
    };

    struct ScriptOwnedResumeValue final
    {
        std::optional<lux::rdesc::ScriptValueType> type;
        std::vector<std::byte> bytes;
    };

    struct ScriptResumePacket final
    {
        ScriptAwaitableId awaitable;
        EScriptAwaitableState state{EScriptAwaitableState::CANCELLED};
        const ScriptOwnedResumeValue* value{};
        ScriptStepError error;
    };

    enum class EScriptAwaitableCompletionError : std::uint8_t
    {
        INVALID_ID,
        INVALID_VALUE,
        ALREADY_TERMINAL,
        RESUME_QUEUE_FULL,
        STOPPING,
    };

    class ScriptAwaitableCompletion final
    {
    public:
        ScriptAwaitableCompletion() = default;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return complete_ != nullptr && instance_.valid() && awaitable_.valid();
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptAwaitableCompletionError> ready(
            ScriptOwnedResumeValue value = {}) const noexcept
        {
            return complete(EScriptAwaitableState::READY, std::move(value), {});
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptAwaitableCompletionError> fail(
            ScriptStepError error) const noexcept
        {
            return complete(EScriptAwaitableState::FAILED, {}, error);
        }

        [[nodiscard]] bool active() const noexcept
        {
            return query_ != nullptr && query_(context_, instance_, awaitable_);
        }

        using CompleteFn = lux::cxx::expected<void, EScriptAwaitableCompletionError> (*)(void*,
                                                                                         ScriptInstanceId,
                                                                                         ScriptAwaitableId,
                                                                                         EScriptAwaitableState,
                                                                                         ScriptOwnedResumeValue,
                                                                                         ScriptStepError) noexcept;
        using QueryFn = bool (*)(void*, ScriptInstanceId, ScriptAwaitableId) noexcept;

        ScriptAwaitableCompletion(std::shared_ptr<void> lease,
                                  void* context,
                                  CompleteFn complete,
                                  QueryFn query,
                                  ScriptInstanceId instance,
                                  ScriptAwaitableId awaitable) noexcept
            : lease_(std::move(lease)), context_(context), complete_(complete), query_(query), instance_(instance),
              awaitable_(awaitable)
        {}

    private:
        [[nodiscard]] lux::cxx::expected<void, EScriptAwaitableCompletionError> complete(
            EScriptAwaitableState state,
            ScriptOwnedResumeValue value,
            ScriptStepError error) const noexcept
        {
            if (complete_ == nullptr)
                return lux::cxx::unexpected<EScriptAwaitableCompletionError>(EScriptAwaitableCompletionError::STOPPING);
            return complete_(context_, instance_, awaitable_, state, std::move(value), error);
        }

        std::shared_ptr<void> lease_;
        void* context_{};
        CompleteFn complete_{};
        QueryFn query_{};
        ScriptInstanceId instance_;
        ScriptAwaitableId awaitable_;
    };

    struct ScriptAwaitableRegistration final
    {
        ScriptAwaitableId id;
        ScriptAwaitableCompletion completion;
    };

    enum class EScriptAwaitableCreateError : std::uint8_t
    {
        INVALID_INSTANCE,
        INVALID_RESULT_TYPE,
        CAPACITY_EXCEEDED,
        ALLOCATION_FAILURE,
        STOPPING,
    };

    class ScriptAwaitableFactory final
    {
    public:
        ScriptAwaitableFactory() = default;
        ScriptAwaitableFactory(const ScriptAwaitableFactory&) = delete;
        ScriptAwaitableFactory& operator=(const ScriptAwaitableFactory&) = delete;
        ScriptAwaitableFactory(ScriptAwaitableFactory&&) = delete;
        ScriptAwaitableFactory& operator=(ScriptAwaitableFactory&&) = delete;

        [[nodiscard]] lux::cxx::expected<ScriptAwaitableRegistration, EScriptAwaitableCreateError> create(
            std::optional<lux::rdesc::ScriptValueType> result_type = std::nullopt) const noexcept
        {
            if (create_ == nullptr)
                return lux::cxx::unexpected<EScriptAwaitableCreateError>(EScriptAwaitableCreateError::STOPPING);
            return create_(context_, instance_, std::move(result_type));
        }

        void discard(ScriptAwaitableId awaitable) const noexcept
        {
            if (discard_ != nullptr && awaitable.valid())
                discard_(context_, instance_, awaitable);
        }

        using CreateFn = lux::cxx::expected<ScriptAwaitableRegistration, EScriptAwaitableCreateError> (*)(
            void*,
            ScriptInstanceId,
            std::optional<lux::rdesc::ScriptValueType>) noexcept;
        using DiscardFn = void (*)(void*, ScriptInstanceId, ScriptAwaitableId) noexcept;

    private:
        ScriptAwaitableFactory(void* context, CreateFn create, DiscardFn discard, ScriptInstanceId instance) noexcept
            : context_(context), create_(create), discard_(discard), instance_(instance)
        {}

        void* context_{};
        CreateFn create_{};
        DiscardFn discard_{};
        ScriptInstanceId instance_;

        friend struct ScriptStepContext;
    };

    struct ScriptStepContext final
    {
        ScriptStepContext(
            ScriptInstanceId instance,
            void* context,
            ScriptAwaitableFactory::CreateFn create,
            ScriptAwaitableFactory::DiscardFn discard
        ) noexcept
            : instance(instance), awaitables(context, create, discard, instance)
        {}

        ScriptStepContext(const ScriptStepContext&) = delete;
        ScriptStepContext& operator=(const ScriptStepContext&) = delete;
        ScriptStepContext(ScriptStepContext&&) = delete;
        ScriptStepContext& operator=(ScriptStepContext&&) = delete;

        ScriptInstanceId instance;
        ScriptAwaitableFactory awaitables;
    };

    struct ScriptBackendContinuation final
    {
        void* state{};
        ScriptStepResult (*resume)(void*, ScriptStepContext&, const ScriptResumePacket&) noexcept {};
        void (*destroy)(void*) noexcept {};

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return state != nullptr && resume != nullptr && destroy != nullptr;
        }
    };
} // namespace lux::simulation::script
