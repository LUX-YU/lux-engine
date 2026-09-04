#pragma once

#include <lux/engine/description/Script.hpp>
#include <lux/engine/function/script/ScriptAbility.hpp>
#include <lux/engine/simulation/SimulationEndpointId.hpp>
#include <lux/engine/simulation/SimulationEndpointSpec.hpp>
#include <lux/engine/system/SystemInstanceId.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <utility>

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

    class ScriptOwnedBytes final
    {
    public:
        static constexpr std::size_t InlineCapacity{32U};

        ScriptOwnedBytes() noexcept = default;
        ScriptOwnedBytes(const ScriptOwnedBytes&) = delete;
        ScriptOwnedBytes& operator=(const ScriptOwnedBytes&) = delete;

        ScriptOwnedBytes(ScriptOwnedBytes&& other) noexcept
        {
            moveFrom(std::move(other));
        }

        ScriptOwnedBytes& operator=(ScriptOwnedBytes&& other) noexcept
        {
            if (this == std::addressof(other))
                return *this;
            reset();
            moveFrom(std::move(other));
            return *this;
        }

        ~ScriptOwnedBytes()
        {
            reset();
        }

        [[nodiscard]] bool resize(
            std::size_t size,
            std::size_t alignment = alignof(std::max_align_t)
        ) noexcept
        {
            const bool is_invalid_alignment = alignment == 0U || (alignment & (alignment - 1U)) != 0U;
            if (is_invalid_alignment)
                return false;
            if (size <= InlineCapacity && alignment <= alignof(std::max_align_t))
            {
                if (size_ != 0U)
                    std::memcpy(inline_.data(), data(), (std::min)(size_, size));
                releaseSpill();
                size_ = size;
                return true;
            }

            const auto allocation_alignment = (std::max)(alignment, alignof(std::max_align_t));
            if (spill_ != nullptr && spill_capacity_ >= size && spill_alignment_ >= allocation_alignment)
            {
                size_ = size;
                return true;
            }
            auto* replacement = static_cast<std::byte*>(
                ::operator new(size, std::align_val_t{allocation_alignment}, std::nothrow)
            );
            if (replacement == nullptr)
                return false;
            if (size_ != 0U)
                std::memcpy(replacement, data(), (std::min)(size_, size));
            releaseSpill();
            spill_ = replacement;
            spill_capacity_ = size;
            spill_alignment_ = allocation_alignment;
            size_ = size;
            return true;
        }

        [[nodiscard]] std::byte* data() noexcept
        {
            return spill_ != nullptr ? spill_ : inline_.data();
        }

        [[nodiscard]] const std::byte* data() const noexcept
        {
            return spill_ != nullptr ? spill_ : inline_.data();
        }

        [[nodiscard]] std::size_t size() const noexcept
        {
            return size_;
        }

        [[nodiscard]] bool empty() const noexcept
        {
            return size_ == 0U;
        }

        [[nodiscard]] std::span<std::byte> span() noexcept
        {
            return {data(), size_};
        }

        [[nodiscard]] std::span<const std::byte> span() const noexcept
        {
            return {data(), size_};
        }

    private:
        void releaseSpill() noexcept
        {
            if (spill_ != nullptr)
                ::operator delete(spill_, std::align_val_t{spill_alignment_});
            spill_ = nullptr;
            spill_capacity_ = 0U;
            spill_alignment_ = alignof(std::max_align_t);
        }

        void reset() noexcept
        {
            releaseSpill();
            size_ = 0U;
        }

        void moveFrom(ScriptOwnedBytes&& other) noexcept
        {
            size_ = std::exchange(other.size_, 0U);
            if (other.spill_ != nullptr)
            {
                spill_ = std::exchange(other.spill_, nullptr);
                spill_capacity_ = std::exchange(other.spill_capacity_, 0U);
                spill_alignment_ = std::exchange(other.spill_alignment_, alignof(std::max_align_t));
            }
            else if (size_ != 0U)
            {
                std::memcpy(inline_.data(), other.inline_.data(), size_);
            }
        }

        alignas(std::max_align_t) std::array<std::byte, InlineCapacity> inline_{};
        std::byte* spill_{};
        std::size_t size_{};
        std::size_t spill_capacity_{};
        std::size_t spill_alignment_{alignof(std::max_align_t)};
    };

    struct PreparedResumeType final
    {
        lux::semantic::TypeId type_id{lux::semantic::InvalidTypeId};
        std::uint32_t size{};
        std::uint32_t alignment{};
        std::uint8_t abi_kind{};

        PreparedResumeType() noexcept = default;

        PreparedResumeType(const lux::rdesc::ScriptValueType& type) noexcept
            : type_id(type.type_id),
              size(type.size),
              alignment(type.alignment),
              abi_kind(type.abi_kind)
        {
        }

        PreparedResumeType& operator=(const lux::rdesc::ScriptValueType& type) noexcept
        {
            type_id = type.type_id;
            size = type.size;
            alignment = type.alignment;
            abi_kind = type.abi_kind;
            return *this;
        }

        [[nodiscard]] constexpr bool valid() const noexcept
        {
            return type_id != lux::semantic::InvalidTypeId && size != 0U && alignment != 0U && abi_kind != 0U;
        }

        [[nodiscard]] constexpr bool matches(const lux::rdesc::ScriptValueType& type) const noexcept
        {
            return valid() && type_id == type.type_id && size == type.size && alignment == type.alignment &&
                abi_kind == type.abi_kind;
        }
    };

    struct ScriptOwnedResumeValue final
    {
        PreparedResumeType type;
        ScriptOwnedBytes bytes;
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

        [[nodiscard]] lux::script::ScriptAbilityErasedCompletion intoAbilityCompletion() && noexcept
        {
            return lux::script::ScriptAbilityErasedCompletion::bind(
                std::move(lease_),
                context_,
                pack(instance_.slot, instance_.generation),
                pack(awaitable_.slot, awaitable_.generation),
                ability_success_,
                ability_failure_,
                ability_active_,
                ability_owner_context_,
                ability_owner_success_,
                ability_owner_failure_
            );
        }

        using CompleteFn = lux::cxx::expected<void, EScriptAwaitableCompletionError> (*)(void*,
                                                                                         ScriptInstanceId,
                                                                                         ScriptAwaitableId,
                                                                                         EScriptAwaitableState,
                                                                                         ScriptOwnedResumeValue,
                                                                                         ScriptStepError) noexcept;
        using QueryFn = bool (*)(void*, ScriptInstanceId, ScriptAwaitableId) noexcept;
        using AbilitySuccessFn = lux::script::ScriptAbilityErasedCompletion::SuccessFn;
        using AbilityFailureFn = lux::script::ScriptAbilityErasedCompletion::FailureFn;
        using AbilityActiveFn = lux::script::ScriptAbilityErasedCompletion::ActiveFn;

        ScriptAwaitableCompletion(std::shared_ptr<void> lease,
                                  void* context,
                                  CompleteFn complete,
                                  QueryFn query,
                                  ScriptInstanceId instance,
                                  ScriptAwaitableId awaitable,
                                  AbilitySuccessFn ability_success,
                                  AbilityFailureFn ability_failure,
                                  AbilityActiveFn ability_active,
                                  void* ability_owner_context = nullptr,
                                  AbilitySuccessFn ability_owner_success = nullptr,
                                  AbilityFailureFn ability_owner_failure = nullptr) noexcept
            : lease_(std::move(lease)), context_(context), complete_(complete), query_(query), instance_(instance),
              awaitable_(awaitable), ability_success_(ability_success), ability_failure_(ability_failure),
              ability_active_(ability_active), ability_owner_context_(ability_owner_context),
              ability_owner_success_(ability_owner_success), ability_owner_failure_(ability_owner_failure)
        {}

    private:
        [[nodiscard]] static constexpr std::uint64_t pack(
            std::uint32_t slot,
            std::uint32_t generation
        ) noexcept
        {
            return (static_cast<std::uint64_t>(slot) << 32U) | generation;
        }

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
        AbilitySuccessFn ability_success_{};
        AbilityFailureFn ability_failure_{};
        AbilityActiveFn ability_active_{};
        void* ability_owner_context_{};
        AbilitySuccessFn ability_owner_success_{};
        AbilityFailureFn ability_owner_failure_{};
    };

    struct ScriptAwaitableRegistration final
    {
        ScriptAwaitableId id;
        ScriptAwaitableCompletion completion;
    };

    [[nodiscard]] constexpr bool supportsExternalResumeLayout(std::size_t size, std::size_t alignment) noexcept
    {
        const bool is_valid_alignment = alignment != 0U && (alignment & (alignment - 1U)) == 0U;
        return size <= ScriptOwnedBytes::InlineCapacity && is_valid_alignment &&
            alignment <= alignof(std::max_align_t);
    }

    enum class EScriptAwaitableCreateError : std::uint8_t
    {
        INVALID_INSTANCE,
        INVALID_RESULT_TYPE,
        EXTERNAL_RESULT_NOT_TRANSPORTABLE,
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

    struct ScriptEventWaitRequest final
    {
        lux::system::SystemInstanceId system;
        EventPointId event;
        EEventRoute route{EEventRoute::SIMULATION_BROADCAST};
    };

    enum class EScriptEventWaitError : std::uint8_t
    {
        INVALID_INSTANCE,
        UNDECLARED_SOURCE,
        ENDPOINT_NOT_FOUND,
        ROUTE_MISMATCH,
        SCOPE_MISMATCH,
        PAYLOAD_NOT_OWNABLE,
        PAYLOAD_TOO_LARGE,
        WAITER_CAPACITY_EXCEEDED,
        AWAITABLE_CAPACITY_EXCEEDED,
        SEQUENCE_EXHAUSTED,
        ALLOCATION_FAILURE,
        STOPPING,
    };

    class ScriptEventWaitFactory final
    {
    public:
        ScriptEventWaitFactory() = default;
        ScriptEventWaitFactory(const ScriptEventWaitFactory&) = delete;
        ScriptEventWaitFactory& operator=(const ScriptEventWaitFactory&) = delete;
        ScriptEventWaitFactory(ScriptEventWaitFactory&&) = delete;
        ScriptEventWaitFactory& operator=(ScriptEventWaitFactory&&) = delete;

        [[nodiscard]] lux::cxx::expected<ScriptAwaitableId, EScriptEventWaitError> wait(
            ScriptEventWaitRequest request
        ) const noexcept
        {
            if (wait_ == nullptr)
                return lux::cxx::unexpected<EScriptEventWaitError>(EScriptEventWaitError::STOPPING);
            return wait_(context_, instance_, request);
        }

        using WaitFn = lux::cxx::expected<ScriptAwaitableId, EScriptEventWaitError> (*)(
            void*,
            ScriptInstanceId,
            ScriptEventWaitRequest
        ) noexcept;

    private:
        ScriptEventWaitFactory(void* context, WaitFn wait, ScriptInstanceId instance) noexcept
            : context_(context), wait_(wait), instance_(instance)
        {
        }

        void* context_{};
        WaitFn wait_{};
        ScriptInstanceId instance_;

        friend struct ScriptStepContext;
    };

    struct ScriptStepContext final
    {
        ScriptStepContext(
            ScriptInstanceId instance,
            void* context,
            ScriptAwaitableFactory::CreateFn create,
            ScriptAwaitableFactory::DiscardFn discard,
            ScriptEventWaitFactory::WaitFn wait_event = nullptr
        ) noexcept
            : instance(instance),
              awaitables(context, create, discard, instance),
              event_waits(context, wait_event, instance)
        {}

        ScriptStepContext(const ScriptStepContext&) = delete;
        ScriptStepContext& operator=(const ScriptStepContext&) = delete;
        ScriptStepContext(ScriptStepContext&&) = delete;
        ScriptStepContext& operator=(ScriptStepContext&&) = delete;

        ScriptInstanceId instance;
        ScriptAwaitableFactory awaitables;
        ScriptEventWaitFactory event_waits;
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
