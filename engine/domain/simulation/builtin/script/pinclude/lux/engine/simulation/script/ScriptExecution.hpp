#pragma once

#include <lux/cxx/container/SlotMap.hpp>
#include <lux/engine/function/script/ScriptAbilityAsync.hpp>
#include <lux/engine/simulation/script/ScriptCompletionIngress.hpp>
#include <lux/engine/simulation/script/ScriptEventWaits.hpp>

#include <lux/engine/simulation/script/ScriptInstances.hpp>
#include <lux/engine/simulation/script/ScriptTimers.hpp>

namespace lux::simulation::script::detail
{
    struct ScriptExecutionFailureSink final
    {
        void* context{};
        void (*fault)(void*, std::uint32_t, lux::script::ScriptSymbolId, EScriptSystemError, std::int32_t) noexcept{};
    };

    class ScriptExecution final
    {
        struct AwaitableRecord;
        struct ExecutionInstance final
        {
            ScriptInstanceId id;
            std::uint32_t mount_slot{};
            std::size_t active_continuations{};
            ScriptContinuationId first_continuation;
            AwaitableRecord* first_awaitable{};
            bool admission_revoked{};
            ScriptInstances::AuthorityAccess authority;
        };

        // Short internal borrow: no directory lookup or writable Instances authority
        // is exposed.
        struct ExecutionAccess final
        {
            ExecutionInstance* record{};
            ScriptInstanceId identity;
            [[nodiscard]] bool current() const noexcept
            {
                return record != nullptr && record->id == identity &&
                    !record->admission_revoked && record->authority.current();
            }
        };

        struct ContinuationRecord final
        {
            ScriptContinuationId id;
            ScriptInstanceId instance;
            ScriptBackendContinuation backend;
            ScriptAwaitableId waiting_on;
            std::uint32_t method_slot{};
            bool hook_single_flight{};
            ScriptContinuationId instance_previous;
            ScriptContinuationId instance_next;
        };

        struct ResumeRecord final
        {
            ScriptInstanceId instance;
            ScriptContinuationId continuation;
            ScriptAwaitableId awaitable;
        };

        struct ResumeRing final
        {
            std::vector<ScriptAwaitableId> records;
            std::size_t head{};
            std::size_t count{};
            std::size_t high_water{};

            void prepare(std::size_t capacity)
            {
                records.resize(capacity);
                head = 0U;
                count = 0U;
                high_water = 0U;
            }

            [[nodiscard]] bool push(ScriptAwaitableId record) noexcept
            {
                if (count >= records.size())
                    return false;
                const auto until_wrap = records.size() - head;
                const auto tail = count < until_wrap ? head + count : count - until_wrap;
                records[tail] = record;
                ++count;
                high_water = (std::max)(high_water, count);
                return true;
            }

            [[nodiscard]] std::optional<ScriptAwaitableId> pop() noexcept
            {
                if (count == 0U)
                    return std::nullopt;
                const auto result = records[head];
                if (++head == records.size())
                    head = 0U;
                --count;
                return result;
            }

            void clear() noexcept
            {
                head = 0U;
                count = 0U;
            }
        };

        struct AwaitableRecord final : ScriptWaitIdentity
        {
            ExecutionInstance* owner{};
            std::uint32_t next_free{};
            ScriptContinuationId continuation;
            EScriptAwaitableState state{EScriptAwaitableState::PENDING};
            std::optional<PreparedResumeType> result_type;
            ScriptOwnedResumeValue value;
            ScriptStepError error;
            bool resume_enqueued{};
            bool external_completion{};
            bool release_pending{};
            std::uint32_t write_pins{};
            AwaitableRecord* instance_previous{};
            AwaitableRecord* instance_next{};
            ScriptWaitSource source;
            ScriptEventWaitLink event_link;
        };

        struct ContinuationTag;
        struct AwaitableTag;
        using Handler = ScriptMethodReference;
        using PreparedMethod = ScriptPreparedMethod;
        using ContinuationStorage = lux::cxx::SlotMap<ContinuationRecord, ContinuationTag>;
        using ContinuationKey = ContinuationStorage::key_type;
        using AwaitableKey = lux::cxx::SlotKey<AwaitableTag>;
        // A fixed result bank, not an additional pool. Records and their Event
        // links live until shutdown. Logical waits acquire a fresh generation and
        // return capacity before user resume; write pins delay that return exactly
        // as before. No per-wait construction, dense index or block lookup remains.
        class AwaitableStorage final
        {
            static constexpr std::uint32_t NoSlot = (std::numeric_limits<std::uint32_t>::max)();
        public:
            void reserve(std::size_t capacity)
            {
                if (active_ != 0U) std::terminate();
                if (records_.empty())
                {
                    records_.resize(capacity);
                    for (auto& record : records_) record.id.generation = 1U;
                }
                else if (records_.size() != capacity) std::terminate();
                rebuildFreeList();
            }
            [[nodiscard]] AwaitableRecord* acquire(ExecutionInstance& owner,
                std::optional<PreparedResumeType> type, bool external) noexcept
            {
                if (free_head_ == NoSlot) return nullptr;
                const auto slot = free_head_;
                auto& record = records_[slot];
                free_head_ = record.next_free;
                record.id.slot = slot + 1U;
                record.instance = owner.id;
                record.owner = &owner;
                record.state = EScriptAwaitableState::PENDING;
                record.result_type = type;
                record.external_completion = external;
                ++active_;
                return &record;
            }
            [[nodiscard]] AwaitableRecord* find(AwaitableKey key) noexcept
            {
                if (key.index >= records_.size()) return nullptr;
                auto& record = records_[key.index];
                return record.id.slot != 0U && record.id.generation == key.gen ? &record : nullptr;
            }
            void releaseResolved(AwaitableRecord& record) noexcept
            {
                const auto slot = record.id.slot - 1U;
                record.id.slot = 0U;
                record.continuation = {};
                record.value = {};
                record.error = {};
                record.resume_enqueued = false;
                record.release_pending = false;
                --active_;
                // Exhausted generations retire the physical slot; never wrap an
                // old public 32-bit token into a newly live wait.
                if (record.id.generation != NoSlot)
                {
                    ++record.id.generation;
                    record.next_free = free_head_;
                    free_head_ = slot;
                }
                else record.id.generation = 0U;
            }
            [[nodiscard]] bool empty() const noexcept { return active_ == 0U; }
            [[nodiscard]] std::size_t size() const noexcept { return active_; }
            [[nodiscard]] std::size_t capacity() const noexcept { return records_.size(); }
            [[nodiscard]] std::size_t storageBytes() const noexcept
            {
                return records_.capacity() * sizeof(AwaitableRecord);
            }
            void clear() noexcept
            {
                if (active_ != 0U) std::terminate();
                // Keep the physical bank and generations for a prepare retry.
                rebuildFreeList();
            }
        private:
            void rebuildFreeList() noexcept
            {
                free_head_ = NoSlot;
                for (std::size_t i = records_.size(); i > 0U; --i)
                {
                    auto& record = records_[i - 1U];
                    if (record.id.generation == 0U) continue;
                    record.next_free = free_head_;
                    free_head_ = static_cast<std::uint32_t>(i - 1U);
                }
            }
            std::vector<AwaitableRecord> records_;
            std::uint32_t free_head_{NoSlot};
            std::size_t active_{};
        };
        struct UserInvocationScope final
        {
            explicit UserInvocationScope(ScriptExecution& owner) noexcept : protection(owner.instance_owner_) {}
            ScriptInstances::Protection protection;
        };
        [[nodiscard]] static constexpr ScriptContinuationId continuationId(ContinuationKey key) noexcept
        {
            return {key.index + 1U, key.gen};
        }
        [[nodiscard]] static constexpr ContinuationKey continuationKey(ScriptContinuationId id) noexcept
        {
            return id.valid() ? ContinuationKey{id.slot - 1U, id.generation} : ContinuationKey::invalid();
        }
        [[nodiscard]] static constexpr ScriptAwaitableId awaitableId(AwaitableKey key) noexcept
        {
            return {key.index + 1U, key.gen};
        }
        [[nodiscard]] static constexpr AwaitableKey awaitableKey(ScriptAwaitableId id) noexcept
        {
            return id.valid() ? AwaitableKey{id.slot - 1U, id.generation} : AwaitableKey::invalid();
        }
        [[nodiscard]] ExecutionInstance* executionRecord(ScriptInstanceId id) noexcept
        {
            if (!id.valid() || id.slot > execution_instances_.size())
                return nullptr;
            auto& record = execution_instances_[id.slot - 1U];
            return record.id == id ? &record : nullptr;
        }

        [[nodiscard]] ExecutionInstance* findExecutionInstance(ScriptInstanceId id) noexcept
        {
            auto* record = executionRecord(id);
            return record != nullptr && record->authority.valid() ? record : nullptr;
        }

        [[nodiscard]] const ExecutionInstance* findExecutionInstance(ScriptInstanceId id) const noexcept
        {
            if (!id.valid() || id.slot > execution_instances_.size())
                return nullptr;
            const auto& record = execution_instances_[id.slot - 1U];
            return record.id == id && record.authority.valid() ? &record : nullptr;
        }

        [[nodiscard]] lux::cxx::expected<AwaitableRecord*, EScriptAwaitableCreateError> reserveAwaitable(
            ExecutionInstance& owner,
            std::optional<PreparedResumeType> result_type,
            bool external_completion
        ) noexcept
        {
            const bool is_invalid_result_type = result_type &&
                (!result_type->valid() ||
                    result_type->size > limits_.max_resume_payload_bytes);
            if (is_invalid_result_type)
            {
                return lux::cxx::unexpected(EScriptAwaitableCreateError::INVALID_RESULT_TYPE);
            }
            if (external_completion && result_type &&
                !supportsExternalResumeLayout(result_type->size, result_type->alignment))
            {
                return lux::cxx::unexpected(EScriptAwaitableCreateError::EXTERNAL_RESULT_NOT_TRANSPORTABLE);
            }
            return admitAwaitable(owner, std::move(result_type), external_completion);
        }

        // Private allocation kernel. Arbitrary result descriptions enter through
        // reserveAwaitable; prepared Events carry the layout proved by Preparer and
        // the current admission lookup.
        [[nodiscard]] lux::cxx::expected<AwaitableRecord*, EScriptAwaitableCreateError>
        admitAwaitable(ExecutionInstance& owner, std::optional<PreparedResumeType> result_type,
                       bool external_completion) noexcept
        {

            if (stopping_)
                return lux::cxx::unexpected(EScriptAwaitableCreateError::STOPPING);
            if (awaitables_.size() >= limits_.awaitable_capacity)
                return lux::cxx::unexpected(EScriptAwaitableCreateError::CAPACITY_EXCEEDED);
            auto* inserted = awaitables_.acquire(owner, std::move(result_type), external_completion);
            if (inserted == nullptr)
                return lux::cxx::unexpected(EScriptAwaitableCreateError::ALLOCATION_FAILURE);
            auto& record = *inserted;
            const auto id = record.id;
            if (!external_completion && record.result_type)
            {
                record.value.type = *record.result_type;
                if (!record.value.bytes.resize(record.result_type->size, record.result_type->alignment))
                {
                    awaitables_.releaseResolved(record);
                    return lux::cxx::unexpected(EScriptAwaitableCreateError::ALLOCATION_FAILURE);
                }
            }
            record.instance_next = owner.first_awaitable;
            if (record.instance_next) record.instance_next->instance_previous = &record;
            owner.first_awaitable = &record;
            if (external_completion)
                ingress_.open(id, record.result_type);
            return &record;
        }
        [[nodiscard]] lux::cxx::expected<ScriptAwaitableId, EScriptAwaitableCreateError> createAwaitableRecord(
            ScriptInstanceId instance, std::optional<PreparedResumeType> result_type) noexcept
        {
            auto* owner = findExecutionInstance(instance);
            if (owner == nullptr)
                return lux::cxx::unexpected(EScriptAwaitableCreateError::INVALID_INSTANCE);
            const auto record = reserveAwaitable(*owner, std::move(result_type), true);
            if (!record)
                return lux::cxx::unexpected(record.error());
            return (*record)->id;
        }
        [[nodiscard]] lux::cxx::expected<ScriptAwaitableRegistration, EScriptAwaitableCreateError>
        createAwaitable(ScriptInstanceId instance, std::optional<PreparedResumeType> type) noexcept
        {
            const auto created = createAwaitableRecord(instance, std::move(type));
            if (!created)
                return lux::cxx::unexpected(created.error());
            return ingress_.registration(instance, *created, this, &ScriptExecution::completeAbilityOwnerErased,
                &ScriptExecution::failAbilityOwnerErased);
        }
        [[nodiscard]] static lux::cxx::expected<ScriptAwaitableRegistration, EScriptAwaitableCreateError>
        createAwaitableErased(void* context,
                              ScriptInstanceId instance,
                              std::optional<PreparedResumeType> result_type) noexcept
        {
            return static_cast<ScriptExecution*>(context)->createAwaitable(instance, std::move(result_type));
        }
        [[nodiscard]] bool validAwaitableOutcome(
            const AwaitableRecord& record,
            EScriptAwaitableState state,
            const ScriptOwnedResumeValue& value,
            ScriptStepError error
        ) const noexcept
        {
            if (state == EScriptAwaitableState::READY)
            {
                const bool is_invalid_value = error.valid() || value.bytes.size() > limits_.max_resume_payload_bytes ||
                    value.type.valid() != record.result_type.has_value();
                if (is_invalid_value)
                    return false;
                if (!record.result_type)
                    return value.bytes.empty();
                return value.type.matches(*record.result_type) && value.bytes.size() == record.result_type->size;
            }
            const bool is_valid_failure = state == EScriptAwaitableState::FAILED && error.valid();
            return is_valid_failure && !value.type.valid() && value.bytes.empty();
        }
        [[nodiscard]] lux::cxx::expected<void, EScriptAwaitableCompletionError> completeAwaitableOwner(
            ScriptInstanceId instance,
            ScriptAwaitableId awaitable,
            EScriptAwaitableState state,
            ScriptOwnedResumeValue value,
            ScriptStepError error
        ) noexcept
        {
            if (stopping_)
                return lux::cxx::unexpected(EScriptAwaitableCompletionError::STOPPING);
            auto* record = awaitables_.find(awaitableKey(awaitable));
            if (record == nullptr || record->instance != instance)
                return lux::cxx::unexpected(EScriptAwaitableCompletionError::INVALID_ID);
            return finishAwaitableOwner(*record, state, &value, error);
        }
        [[nodiscard]] lux::cxx::expected<void, EScriptAwaitableCompletionError> finishAwaitableOwner(
            AwaitableRecord& record, EScriptAwaitableState state, ScriptOwnedResumeValue* supplied,
            ScriptStepError error
        ) noexcept
        {
            if (stopping_) return lux::cxx::unexpected(EScriptAwaitableCompletionError::STOPPING);
            if (record.state != EScriptAwaitableState::PENDING || record.release_pending)
                return lux::cxx::unexpected(EScriptAwaitableCompletionError::ALREADY_TERMINAL);
            if (!validAwaitableOutcome(record, state, supplied ? *supplied : record.value, error))
                return lux::cxx::unexpected(EScriptAwaitableCompletionError::INVALID_VALUE);
            if (record.continuation.valid() && resumes_.count >= resumes_.records.size())
                return lux::cxx::unexpected(EScriptAwaitableCompletionError::RESUME_QUEUE_FULL);
            record.state = state;
            if (supplied != nullptr) record.value = std::move(*supplied);
            record.error = error;
            releaseSource(record);
            if (record.external_completion)
                ingress_.close(record.id);
            if (record.continuation.valid())
            {
                static_cast<void>(resumes_.push(record.id));
                record.resume_enqueued = true;
            }
            return {};
        }
        [[nodiscard]] static lux::cxx::expected<void, lux::script::EScriptAbilityCompletionError>
        completeAbilityOwnerErased(
            void* context,
            std::uint64_t instance_value,
            std::uint64_t awaitable_value,
            lux::semantic::TypeId type,
            const void* data,
            std::uint32_t size
        ) noexcept
        {
            auto& owner = *static_cast<ScriptExecution*>(context);
            const auto instance = ScriptCompletionIngress::unpackInstance(instance_value);
            const auto awaitable = ScriptCompletionIngress::unpackAwaitable(awaitable_value);
            auto* record = owner.awaitables_.find(awaitableKey(awaitable));
            if (record == nullptr || record->instance != instance)
                return lux::cxx::unexpected(lux::script::EScriptAbilityCompletionError::STALE);

            ScriptOwnedResumeValue value;
            if (record->result_type)
            {
                const auto& expected = *record->result_type;
                if (data == nullptr || type != expected.type_id || size != expected.size ||
                    !value.bytes.resize(size, expected.alignment))
                {
                    return lux::cxx::unexpected(lux::script::EScriptAbilityCompletionError::INVALID_VALUE);
                }
                value.type = expected;
                std::memcpy(value.bytes.data(), data, size);
            }
            else if (type != lux::semantic::InvalidTypeId || data != nullptr || size != 0U)
            {
                return lux::cxx::unexpected(lux::script::EScriptAbilityCompletionError::INVALID_VALUE);
            }

            const auto completed = owner.finishAwaitableOwner(*record, EScriptAwaitableState::READY, &value, {});
            return completed
                ? lux::cxx::expected<void, lux::script::EScriptAbilityCompletionError>{}
                : lux::cxx::unexpected(ScriptCompletionIngress::abilityError(completed.error()));
        }
        [[nodiscard]] static lux::cxx::expected<void, lux::script::EScriptAbilityCompletionError>
        failAbilityOwnerErased(
            void* context,
            std::uint64_t instance_value,
            std::uint64_t awaitable_value,
            lux::script::ScriptAbilityOperationError error
        ) noexcept
        {
            auto& owner = *static_cast<ScriptExecution*>(context);
            const auto completed = owner.completeAwaitableOwner(
                ScriptCompletionIngress::unpackInstance(instance_value),
                ScriptCompletionIngress::unpackAwaitable(awaitable_value),
                EScriptAwaitableState::FAILED,
                {},
                {error.status}
            );
            return completed
                ? lux::cxx::expected<void, lux::script::EScriptAbilityCompletionError>{}
                : lux::cxx::unexpected(ScriptCompletionIngress::abilityError(completed.error()));
        }
        [[nodiscard]] static EScriptEventWaitError eventWaitError(EScriptAwaitableCreateError error) noexcept
        {
            switch (error)
            {
            case EScriptAwaitableCreateError::INVALID_INSTANCE:
                return EScriptEventWaitError::INVALID_INSTANCE;
            case EScriptAwaitableCreateError::INVALID_RESULT_TYPE:
            case EScriptAwaitableCreateError::EXTERNAL_RESULT_NOT_TRANSPORTABLE:
                return EScriptEventWaitError::PAYLOAD_NOT_OWNABLE;
            case EScriptAwaitableCreateError::CAPACITY_EXCEEDED:
                return EScriptEventWaitError::AWAITABLE_CAPACITY_EXCEEDED;
            case EScriptAwaitableCreateError::ALLOCATION_FAILURE:
                return EScriptEventWaitError::ALLOCATION_FAILURE;
            case EScriptAwaitableCreateError::STOPPING:
                return EScriptEventWaitError::STOPPING;
            }
            return EScriptEventWaitError::ALLOCATION_FAILURE;
        }
        [[nodiscard]] lux::cxx::expected<ScriptAwaitableId, EScriptEventWaitError> waitEvent(
            ScriptInstanceId instance,
            ScriptEventAdmissionHandle admission
        ) noexcept
        {
            if (stopping_ || !prepared_)
                return lux::cxx::unexpected(EScriptEventWaitError::STOPPING);
            auto* owner = executionRecord(instance);
            if (owner == nullptr)
                return lux::cxx::unexpected(EScriptEventWaitError::INVALID_INSTANCE);
            const auto source = instance_owner_.eventSource(instance, owner->mount_slot, admission);
            if (!source)
                return lux::cxx::unexpected(source.error());
            const auto endpoint_slot = source->source->endpoint_slot;
            const auto& endpoint = binding_owner_.eventEndpoint(endpoint_slot);
            ecs::Entity target{ecs::NullEntity};
            if (endpoint.route == EEventRoute::ENTITY_TARGETED)
            {
                const auto* entity = std::get_if<EntityScriptScope>(source->scope);
                if (entity == nullptr || entity->self == ecs::NullEntity || !instance_owner_.validEntity(entity->self))
                    return lux::cxx::unexpected(EScriptEventWaitError::SCOPE_MISMATCH);
                target = entity->self;
            }

            auto reservation = event_owner_.reserve(instance, endpoint_slot, target);
            if (!reservation)
                return lux::cxx::unexpected(reservation.error());

            // No user code or owner mutation can intervene before waiter commit. The
            // authoritative incarnation/source check above covers result admission as
            // well; storage has stable addresses.
            auto awaitable = admitAwaitable(*owner, source->source->payload, false);
            if (!awaitable)
                return lux::cxx::unexpected(eventWaitError(awaitable.error()));

            auto& record = **awaitable;
            const auto source_id = event_owner_.registerWait(std::move(*reservation), record.event_link, record);
            record.source = {source_id, EScriptWaitSource::EVENT};
            return record.id;
        }
        [[nodiscard]] static lux::cxx::expected<ScriptAwaitableId, EScriptEventWaitError> waitEventErased(
            void* context,
            ScriptInstanceId instance,
            ScriptEventAdmissionHandle admission
        ) noexcept
        {
            return static_cast<ScriptExecution*>(context)->waitEvent(instance, admission);
        }
        [[nodiscard]] lux::cxx::expected<void, EScriptSystemError> attachWaiter(
            ScriptAwaitableId awaitable,
            ScriptInstanceId instance,
            ScriptContinuationId continuation) noexcept
        {
            auto* record = awaitables_.find(awaitableKey(awaitable));
            if (record == nullptr || record->instance != instance || record->continuation.valid())
                return lux::cxx::unexpected(EScriptSystemError::INVOCATION_FAILURE);
            if (record->state != EScriptAwaitableState::PENDING &&
                resumes_.count >= resumes_.records.size())
            {
                return lux::cxx::unexpected(EScriptSystemError::RESUME_QUEUE_FULL);
            }
            record->continuation = continuation;
            if (record->state != EScriptAwaitableState::PENDING)
            {
                static_cast<void>(resumes_.push(awaitable));
                record->resume_enqueued = true;
            }
            return {};
        }
        struct AwaitableOutcome final
        {
            AwaitableOutcome(EScriptAwaitableState result_state, ScriptOwnedResumeValue&& result,
                ScriptStepError result_error) noexcept
                : state(result_state), value(std::move(result)), error(result_error) {}
            EScriptAwaitableState state{EScriptAwaitableState::CANCELLED};
            ScriptOwnedResumeValue value;
            ScriptStepError error;
        };

        void unlinkAwaitableOwnership(AwaitableRecord& record, ExecutionAccess access) noexcept
        {
            auto* owner = access.record;
            if (record.instance_previous) record.instance_previous->instance_next = record.instance_next;
            else if (owner != nullptr && owner->first_awaitable == &record)
                owner->first_awaitable = record.instance_next;
            if (record.instance_next) record.instance_next->instance_previous = record.instance_previous;
            record.instance_previous = nullptr;
            record.instance_next = nullptr;
        }
        [[nodiscard]] bool eraseAwaitable(ScriptAwaitableId id) noexcept
        {
            auto* record = awaitables_.find(awaitableKey(id));
            if (record == nullptr)
                return false;
            return eraseAwaitableRecord(*record, {record->owner, record->instance});
        }
        [[nodiscard]] bool eraseAwaitableRecord(AwaitableRecord& value, ExecutionAccess access) noexcept
        {
            auto* record = &value;
            const auto id = record->id;
            if (record->release_pending) return true;
            releaseSource(*record);
            unlinkAwaitableOwnership(*record, access);
            if (record->external_completion)
                ingress_.close(id);
            if (record->write_pins != 0U)
            {
                record->state = EScriptAwaitableState::CANCELLED;
                record->release_pending = true;
                ++pending_awaitable_releases_;
                return true;
            }
            awaitables_.releaseResolved(*record);
            return true;
        }
        struct ResultWritePin final
        {
            ScriptExecution& owner;
            AwaitableRecord& record;
            ResultWritePin(ScriptExecution& state, AwaitableRecord& value) noexcept : owner(state), record(value)
            {
                ++record.write_pins;
                ++owner.result_write_pins_;
            }
            ~ResultWritePin() noexcept
            {
                --record.write_pins;
                --owner.result_write_pins_;
                if (record.write_pins == 0U && record.release_pending)
                {
                    --owner.pending_awaitable_releases_;
                    owner.awaitables_.releaseResolved(record);
                }
            }
        };

        [[nodiscard]] bool takeAwaitable(
            AwaitableRecord& value, ExecutionAccess access, std::optional<AwaitableOutcome>& outcome
        ) noexcept
        {
            auto* record = &value;
            if (record->state != EScriptAwaitableState::READY && record->state != EScriptAwaitableState::FAILED)
                return false;
            outcome.emplace(record->state, std::move(record->value), record->error);
            static_cast<void>(eraseAwaitableRecord(*record, access));
            return true;
        }
        void cancelAwaitables(ScriptInstanceId instance, AwaitableRecord* first) noexcept
        {
            const ExecutionAccess access{executionRecord(instance), instance};
            auto* current = first;
            while (current)
            {
                auto* next = current->instance_next;
                static_cast<void>(eraseAwaitableRecord(*current, access));
                ++instance_cleanup_awaitable_visits_;
                current = next;
            }
        }
        void releaseSource(AwaitableRecord& record) noexcept
        {
            const auto source = std::exchange(record.source, {});
            if (source.kind == EScriptWaitSource::EVENT)
                static_cast<void>(event_owner_.cancel(record.event_link));
            else if (source.kind == EScriptWaitSource::TIMER)
                static_cast<void>(timer_owner_.cancel(source.id));
        }
        void discardAwaitable(ScriptInstanceId instance, ScriptAwaitableId id) noexcept
        {
            auto* record = awaitables_.find(awaitableKey(id));
            if (record != nullptr && record->instance == instance)
                static_cast<void>(eraseAwaitableRecord(*record, {record->owner, instance}));
        }
        static void discardAwaitableErased(
            void* context,
            ScriptInstanceId instance,
            ScriptAwaitableId awaitable
        ) noexcept
        {
            static_cast<ScriptExecution*>(context)->discardAwaitable(instance, awaitable);
        }
        void clearActiveHook(const ContinuationRecord& continuation) noexcept
        {
            if (!continuation.hook_single_flight || continuation.method_slot >= active_hooks_.size())
                return;
            auto& flight = active_hooks_[continuation.method_slot];
            if (flight.instance == continuation.instance && flight.continuation == continuation.id)
            {
                flight = {};
                binding_owner_.setMethodRunnable(continuation.method_slot, continuation.instance, true);
            }
        }
        void destroyContinuation(ScriptContinuationId id) noexcept
        {
            auto* stored = continuations_.find(continuationKey(id));
            if (stored == nullptr)
                return;
            const auto continuation = *stored;
            if (auto* instance = executionRecord(continuation.instance); instance != nullptr)
            {
                if (instance->active_continuations == 0U)
                    std::terminate();
                --instance->active_continuations;
                if (!continuation.instance_previous.valid() && instance->first_continuation == id)
                    instance->first_continuation = continuation.instance_next;
            }
            if (continuation.instance_previous.valid())
            {
                auto* previous = continuations_.find(continuationKey(continuation.instance_previous));
                if (previous != nullptr)
                    previous->instance_next = continuation.instance_next;
            }
            if (continuation.instance_next.valid())
            {
                auto* next = continuations_.find(continuationKey(continuation.instance_next));
                if (next != nullptr)
                    next->instance_previous = continuation.instance_previous;
            }
            clearActiveHook(continuation);
            static_cast<void>(continuations_.erase(continuationKey(id)));
            UserInvocationScope cleanup(*this);
            continuation.backend.destroy(continuation.backend.state);
        }
        void destroyContinuations(ScriptInstanceId instance, ScriptContinuationId first) noexcept
        {
            auto current = first;
            while (current.valid())
            {
                const auto* record = continuations_.find(continuationKey(current));
                if (record == nullptr || record->instance != instance)
                    std::terminate();
                const auto next = record->instance_next;
                destroyContinuation(current);
                ++instance_cleanup_continuation_visits_;
                current = next;
            }
        }
        void faultInvocation(std::uint32_t slot, lux::script::ScriptSymbolId symbol,
            EScriptSystemError error, std::int32_t status = 0) noexcept
        {
            failures_.fault(failures_.context, slot, symbol, error, status);
        }
        [[nodiscard]] bool beginSuspension(
            std::uint32_t mount_slot, std::uint32_t method_slot, ScriptInstanceId instance_id,
            const PreparedMethod& method, ScriptBackendContinuation backend_continuation,
            ScriptStepResult result, bool hook_single_flight
        ) noexcept
        {
            // The caller just revalidated its Invocation after backend code; no user
            // code intervenes before this helper. Keep that protected full identity,
            // not a cold mount snapshot.
            if (!result.valid() || result.state != EScriptStepState::SUSPENDED || !backend_continuation)
            {
                if (backend_continuation)
                    backend_continuation.destroy(backend_continuation.state);
                discardAwaitable(instance_id, result.waiting_on);
                faultInvocation(mount_slot, method.symbol, EScriptSystemError::INVOCATION_FAILURE);
                return false;
            }
            auto* instance = executionRecord(instance_id);
            if (instance == nullptr)
            {
                backend_continuation.destroy(backend_continuation.state);
                discardAwaitable(instance_id, result.waiting_on);
                faultInvocation(mount_slot, method.symbol, EScriptSystemError::INVOCATION_FAILURE);
                return false;
            }
            if (instance->active_continuations >= limits_.continuation_capacity_per_instance)
            {
                backend_continuation.destroy(backend_continuation.state);
                discardAwaitable(instance_id, result.waiting_on);
                faultInvocation(
                    mount_slot,
                    method.symbol,
                    EScriptSystemError::INSTANCE_CONTINUATION_CAPACITY_EXCEEDED
                );
                return false;
            }
            if (continuations_.size() >= limits_.continuation_capacity)
            {
                backend_continuation.destroy(backend_continuation.state);
                discardAwaitable(instance_id, result.waiting_on);
                faultInvocation(mount_slot, method.symbol, EScriptSystemError::CONTINUATION_CAPACITY_EXCEEDED);
                return false;
            }
            auto inserted = continuations_.tryEmplace(
                ContinuationRecord{{},
                                   instance_id,
                                   backend_continuation,
                                   result.waiting_on,
                                   method_slot,
                                   hook_single_flight});
            if (!inserted)
            {
                backend_continuation.destroy(backend_continuation.state);
                discardAwaitable(instance_id, result.waiting_on);
                faultInvocation(mount_slot, method.symbol, EScriptSystemError::ALLOCATION_FAILURE);
                return false;
            }
            const auto id = continuationId(*inserted);
            auto& stored = continuations_[*inserted];
            stored.id = id;
            stored.instance_next = instance->first_continuation;
            if (stored.instance_next.valid())
            {
                auto* next = continuations_.find(continuationKey(stored.instance_next));
                if (next == nullptr)
                    std::terminate();
                next->instance_previous = id;
            }
            instance->first_continuation = id;
            ++instance->active_continuations;
            auto attached = attachWaiter(result.waiting_on, instance_id, id);
            if (!attached)
            {
                destroyContinuation(id);
                discardAwaitable(instance_id, result.waiting_on);
                faultInvocation(mount_slot, method.symbol, attached.error());
                return false;
            }
            if (hook_single_flight)
            {
                active_hooks_[method_slot] = {instance_id, id};
                binding_owner_.setMethodRunnable(method_slot, instance_id, false);
            }
            ++suspensions_admitted_;
            return true;
        }
        void failEventWaiter(ScriptInstanceId instance, EScriptSystemError error) noexcept
        {
            const auto* owner = findExecutionInstance(instance);
            if (owner != nullptr && owner->authority.current())
                faultInvocation(owner->mount_slot, lux::script::InvalidScriptSymbolId, error);
        }
        [[nodiscard]] lux::cxx::expected<void, EScriptSystemError> resumeOne(ScriptAwaitableId ready) noexcept
        {
            auto* wait = awaitables_.find(awaitableKey(ready));
            if (wait == nullptr) return {};
            const ResumeRecord resume{wait->instance, wait->continuation, ready};
            auto* instance = wait->owner;
            auto* continuation = continuations_.find(continuationKey(resume.continuation));
            if (instance == nullptr || continuation == nullptr || continuation->instance != resume.instance ||
                continuation->waiting_on != resume.awaitable)
            {
                return {};
            }
            const ExecutionAccess execution{instance, resume.instance};
            std::optional<AwaitableOutcome> outcome;
            if (!takeAwaitable(*wait, execution, outcome))
                return {};
            if (!execution.current())
            {
                destroyContinuation(resume.continuation);
                return {};
            }

            continuation->waiting_on = {};
            ScriptResumePacket packet{resume.awaitable, outcome->state, std::addressof(outcome->value), outcome->error};
            ScriptStepContext context{
                resume.instance,
                this,
                &ScriptExecution::createAwaitableErased,
                &ScriptExecution::discardAwaitableErased,
                &ScriptExecution::waitEventErased
            };
            const auto result = [&]() noexcept {
                ++backend_resume_calls_;
                return continuation->backend.resume(continuation->backend.state, context, packet);
            }();

            continuation = continuations_.find(continuationKey(resume.continuation));
            if (continuation == nullptr)
                return {};
            if (!execution.current())
            {
                destroyContinuation(resume.continuation);
                return {};
            }
            const auto slot = instance->mount_slot;
            if (result.state == EScriptStepState::COMPLETED && result.valid())
            {
                destroyContinuation(resume.continuation);
                return {};
            }
            if (result.state == EScriptStepState::SUSPENDED && result.valid())
            {
                continuation->waiting_on = result.waiting_on;
                auto attached = attachWaiter(result.waiting_on, resume.instance, resume.continuation);
                if (attached)
                {
                    ++suspensions_admitted_;
                    return {};
                }
                const auto error = attached.error();
                const auto symbol = instance_owner_.methodSymbol(continuation->method_slot);
                destroyContinuation(resume.continuation);
                discardAwaitable(resume.instance, result.waiting_on);
                faultInvocation(slot, symbol, error);
                return lux::cxx::unexpected(error);
            }

            const auto status = result.error.status;
            const auto symbol = instance_owner_.methodSymbol(continuation->method_slot);
            destroyContinuation(resume.continuation);
            faultInvocation(slot, symbol, EScriptSystemError::INVOCATION_FAILURE, status);
            return lux::cxx::unexpected(EScriptSystemError::INVOCATION_FAILURE);
        }
    public:
        using Result = lux::cxx::expected<void, EScriptSystemError>;
        ScriptExecution(ScriptInstances& instances, ScriptBindings& bindings, ScriptEventWaits& events,
            ScriptTimers& timers, ScriptCompletionIngress& ingress) noexcept
            : instance_owner_(instances), binding_owner_(bindings), event_owner_(events),
              timer_owner_(timers), ingress_(ingress) {}
        ScriptExecution(const ScriptExecution&) = delete;
        ScriptExecution& operator=(const ScriptExecution&) = delete;

        void prepare(ScriptRuntimeLimits configured, std::size_t instance_capacity,
            std::size_t method_capacity, ScriptExecutionFailureSink failures);
        void beginInstance(ScriptInstanceId instance, std::uint32_t slot) noexcept
        {
            execution_instances_[instance.slot - 1U] = {instance, slot};
            execution_instances_[instance.slot - 1U].authority = instance_owner_.authorityAccess(instance, slot);
        }
        void enablePrepared() noexcept { prepared_ = true; }
        void stop() noexcept { stopping_ = true; }
        [[nodiscard]] std::size_t resultPins() const noexcept { return result_write_pins_; }
        [[nodiscard]] std::size_t activeContinuations() const noexcept { return continuations_.size(); }
        [[nodiscard]] std::size_t activeAwaitables() const noexcept
        {
            return awaitables_.size() - pending_awaitable_releases_;
        }
        [[nodiscard]] std::size_t physicalAwaitableCapacity() const noexcept { return awaitables_.capacity(); }

        [[nodiscard]] lux::cxx::expected<ScriptTimerAdmission, EScriptAwaitableCreateError>
        reserveLocalTimer(const ScriptStepContext& context) noexcept
        {
            auto* owner = findExecutionInstance(context.instance);
            if (!context.awaitables.belongsTo(this) || owner == nullptr || !owner->authority.current())
                return lux::cxx::unexpected(EScriptAwaitableCreateError::INVALID_INSTANCE);
            const auto record = admitAwaitable(*owner, std::nullopt, false);
            if (!record) return lux::cxx::unexpected(record.error());
            return ScriptTimerAdmission{{context.instance, (*record)->id}, *record};
        }
        void discardLocalTimer(ScriptTimerAssociation association) noexcept
        {
            discardAwaitable(association.instance, association.awaitable);
        }
        [[nodiscard]] lux::cxx::expected<void, lux::script::EScriptAbilityCompletionError>
        completeLocalTimer(ScriptTimerAssociation association, ScriptSourceId source) noexcept
        {
            auto* owner = findExecutionInstance(association.instance);
            auto* record = awaitables_.find(awaitableKey(association.awaitable));
            const bool invalid = owner == nullptr || !owner->authority.current() || record == nullptr ||
                record->instance != association.instance || record->external_completion || record->result_type ||
                record->source != ScriptWaitSource{source, EScriptWaitSource::TIMER};
            if (invalid) return lux::cxx::unexpected(lux::script::EScriptAbilityCompletionError::STALE);
            const auto completed = finishAwaitableOwner(*record, EScriptAwaitableState::READY, nullptr, {});
            return completed ? lux::cxx::expected<void, lux::script::EScriptAbilityCompletionError>{} :
                lux::cxx::unexpected(ScriptCompletionIngress::abilityError(completed.error()));
        }

        [[nodiscard]] std::optional<ScriptTimerAdmission> timerAssociation(
            const lux::script::ScriptAbilityCompletion<void>& completion) noexcept
        {
            std::uint64_t a{}, b{};
            if (!lux::script::detail::ScriptAbilityOwnerCompletionAccess::matchOwner(completion, this, a, b))
                return std::nullopt;
            const auto instance = ScriptCompletionIngress::unpackInstance(a);
            const auto id = ScriptCompletionIngress::unpackAwaitable(b);
            auto* record = awaitables_.find(awaitableKey(id));
            const auto* owner = findExecutionInstance(instance);
            const bool matches = !stopping_ && owner != nullptr && owner->authority.current() && record != nullptr &&
                record->instance == instance && record->state == EScriptAwaitableState::PENDING &&
                !record->release_pending && record->source.kind == EScriptWaitSource::NONE;
            return matches ? std::optional{ScriptTimerAdmission{{instance, id}, record}} : std::nullopt;
        }
        void attachTimer(const ScriptTimerAdmission& admission, ScriptSourceId id) noexcept
        {
            // Timer storage is distinct and preallocated. Registration executes no user
            // code and cannot mutate awaitables or authority; source commit needs no
            // second identity lookup.
            static_cast<AwaitableRecord*>(admission.result_)->source = {id, EScriptWaitSource::TIMER};
        }
        void detachSource(ScriptSourceCancellation cancelled) noexcept
        {
            auto* record = awaitables_.find(awaitableKey(cancelled.awaitable));
            if (record != nullptr && record->instance == cancelled.instance && record->source == cancelled.source)
                record->source = {};
        }
        void invalidateAdmission(ScriptInstanceId instance) noexcept
        {
            if (!instance.valid())
                return;
            auto* record = executionRecord(instance);
            if (record == nullptr || record->admission_revoked)
                return;
            record->admission_revoked = true;
            const auto first_awaitable = record->first_awaitable;
            UserInvocationScope cleanup(*this);
            while (const auto cancelled = event_owner_.cancelNext(instance))
                detachSource(*cancelled);
            while (const auto cancelled = timer_owner_.cancelNext(instance))
                detachSource(*cancelled);
            cancelAwaitables(instance, first_awaitable);
        }
        void invalidateInstance(ScriptInstanceId retired) noexcept
        {
            invalidateAdmission(retired);
            auto* record = executionRecord(retired);
            if (record == nullptr)
                return;
            const auto first = record->first_continuation;
            *record = {}; // Claim execution teardown before entering a continuation
                          // destructor.
            UserInvocationScope cleanup(*this);
            destroyContinuations(retired, first);
        }
        [[nodiscard]] bool prepareInvocation(Handler& handler) noexcept
        {
            bool resumable{};
            handler.prepared = instance_owner_.prepareInvocation(handler, resumable);
            if (handler.prepared == nullptr)
                return false;
            handler.entry = resumable ? &invokeStepEntry : &invokeSyncEntry;
            return true;
        }
        void invoke(const Handler& handler, lux_script_call_frame& frame, bool hook_invocation) noexcept
        {
            handler.entry(*this, handler, frame, hook_invocation);
        }
    private:
        static void invokeSyncEntry(ScriptExecution& owner, const Handler& handler,
            lux_script_call_frame& frame, bool) noexcept
        {
            owner.invokeSync(handler, frame);
        }
        static void invokeStepEntry(ScriptExecution& owner, const Handler& handler,
            lux_script_call_frame& frame, bool hook) noexcept
        {
            owner.invokeStep(handler, frame, hook);
        }
        void invokeSync(const Handler& handler, lux_script_call_frame& frame) noexcept
        {
            const auto& access = *handler.prepared;
            if (!access.current())
                return;
            const auto& call = access.synchronous();

            const auto status = [&]() noexcept {
                ++sync_invocations_;
                return call.invoke(call.context, &frame);
            }();
            if (status == 0)
                return;
            // Revoking new calls must not discard an error returned by this protected
            // incarnation.
            if (!access.sameIncarnation())
                return;
            faultInvocation(handler.mount_slot, instance_owner_.methodSymbol(handler.method_slot),
                EScriptSystemError::INVOCATION_FAILURE, status);
        }
        void invokeStep(const Handler& handler, lux_script_call_frame& frame, bool hook_invocation) noexcept
        {
            const auto& access = *handler.prepared;
            if (!access.current())
                return;
            const auto& method = access.resumableMethod();
            ScriptBackendContinuation continuation;
            ScriptStepContext context{
                handler.instance,
                this,
                &ScriptExecution::createAwaitableErased,
                &ScriptExecution::discardAwaitableErased,
                &ScriptExecution::waitEventErased
            };
            const auto result = [&]() noexcept {
                ++step_invocations_;
                return method.backend.resumable.invoke(
                    method.backend.resumable.context, frame, context, continuation);
            }();
            if (!access.current())
            {
                if (continuation)
                    continuation.destroy(continuation.state);
                discardAwaitable(context.instance, result.waiting_on);
                return;
            }
            if (result.state == EScriptStepState::COMPLETED && result.valid())
            {
                if (continuation)
                {
                    continuation.destroy(continuation.state);
                    faultInvocation(handler.mount_slot, method.symbol, EScriptSystemError::INVOCATION_FAILURE);
                }
                return;
            }
            if (result.state == EScriptStepState::SUSPENDED)
            {
                static_cast<void>(beginSuspension(
                    handler.mount_slot, handler.method_slot, access.instance(),
                    method, continuation, result, hook_invocation
                ));
                return;
            }
            if (continuation)
                continuation.destroy(continuation.state);
            faultInvocation(
                handler.mount_slot, method.symbol, EScriptSystemError::INVOCATION_FAILURE, result.error.status
            );
            return;
        }
        // A prepared Event has already copied into its owned layout and released
        // its source. Copy may reenter: terminal state and queue capacity are dynamic.
        [[nodiscard]] lux::cxx::expected<void, EScriptAwaitableCompletionError>
        finishPreparedEvent(AwaitableRecord& record) noexcept
        {
            if (stopping_)
                return lux::cxx::unexpected(EScriptAwaitableCompletionError::STOPPING);
            if (record.state != EScriptAwaitableState::PENDING || record.release_pending)
                return lux::cxx::unexpected(EScriptAwaitableCompletionError::ALREADY_TERMINAL);
            if (record.continuation.valid() && resumes_.count >= resumes_.records.size())
                return lux::cxx::unexpected(EScriptAwaitableCompletionError::RESUME_QUEUE_FULL);
            record.state = EScriptAwaitableState::READY;
            record.error = {};
            if (record.continuation.valid())
            {
                static_cast<void>(resumes_.push(record.id));
                record.resume_enqueued = true;
            }
            return {};
        }
    public:
        void completeClaimedEventWaiter(const ScriptClaimedEventWait& waiter, lux_script_call_frame& frame) noexcept
        {
            const ScriptSourceId id{waiter.awaitable.slot, waiter.awaitable.generation};
            const auto awaitable = waiter.awaitable;
            auto* record = awaitables_.find(awaitableKey(awaitable));
            const bool is_stale = record == nullptr || record->release_pending ||
                record->source != ScriptWaitSource{id, EScriptWaitSource::EVENT};
            if (is_stale) return;
            const auto instance = record->instance;
            auto* owner = record->owner;
            const ExecutionAccess execution{owner, instance};
            if (!execution.current())
            {
                static_cast<void>(eraseAwaitableRecord(*record, execution));
                return;
            }
            const auto& endpoint = binding_owner_.eventEndpoint(waiter.endpoint);
            ResultWritePin pin(*this, *record);
            const bool is_invalid_frame = frame.arg_count != 1U || frame.args == nullptr;
            bool copied{};
            {
                UserInvocationScope invocation(*this);
                copied = !is_invalid_frame && endpoint.payload_projection.copy(
                    endpoint.context, frame.args[0], record->value.bytes.span());
            }
            const bool still_live = !stopping_ && !record->release_pending && execution.current();
            if (!copied || !still_live)
            {
                static_cast<void>(eraseAwaitableRecord(*record, execution));
                if (still_live) faultInvocation(owner->mount_slot, lux::script::InvalidScriptSymbolId,
                    EScriptSystemError::INVOCATION_FAILURE);
                return;
            }
            event_payload_copy_bytes_ += record->value.bytes.size();
            releaseSource(*record);
            const auto completed = finishPreparedEvent(*record);
            if (completed)
                return;

            static_cast<void>(eraseAwaitableRecord(*record, execution));
            switch (completed.error())
            {
            case EScriptAwaitableCompletionError::INVALID_ID:
            case EScriptAwaitableCompletionError::ALREADY_TERMINAL:
            case EScriptAwaitableCompletionError::STOPPING:
                return;
            case EScriptAwaitableCompletionError::RESUME_QUEUE_FULL:
                failEventWaiter(instance, EScriptSystemError::RESUME_QUEUE_FULL);
                return;
            case EScriptAwaitableCompletionError::INVALID_VALUE:
                failEventWaiter(instance, EScriptSystemError::INVOCATION_FAILURE);
                return;
            }
        }
        [[nodiscard]] bool hasPendingExternalCompletions() const noexcept { return ingress_.hasPendingInWindow(); }
        [[nodiscard]] bool drainExternalCompletions() noexcept
        {
            while (const auto* external = ingress_.peek())
            {
                auto* record = awaitables_.find(awaitableKey(external->awaitable));
                const bool is_stale = record == nullptr || record->instance != external->instance;
                if (is_stale)
                {
                    ingress_.ack();
                    continue;
                }
                if (record->continuation.valid() && resumes_.count >= resumes_.records.size())
                    return true;

                ScriptOwnedResumeValue value;
                const bool expects_value = record->result_type.has_value();
                const bool has_value = external->type != lux::semantic::InvalidTypeId;
                const bool is_invalid_external_value = expects_value != has_value ||
                    (expects_value && (external->type != record->result_type->type_id ||
                        external->size != record->result_type->size));
                if (external->state == EScriptAwaitableState::READY && is_invalid_external_value)
                {
                    const auto instance = external->instance;
                    const auto awaitable = external->awaitable;
                    ingress_.ack();
                    discardAwaitable(instance, awaitable);
                    return false;
                }
                if (external->state == EScriptAwaitableState::READY && record->result_type)
                {
                    value.type = *record->result_type;
                    if (!value.bytes.resize(record->result_type->size, record->result_type->alignment))
                        return false;
                    std::memcpy(value.bytes.data(), external->bytes.data(), external->size);
                }
                const auto completed = completeAwaitableOwner(
                    external->instance,
                    external->awaitable,
                    external->state,
                    std::move(value),
                    external->error
                );
                if (!completed && completed.error() == EScriptAwaitableCompletionError::RESUME_QUEUE_FULL)
                    return true;
                ingress_.ack();
                if (!completed && completed.error() != EScriptAwaitableCompletionError::INVALID_ID &&
                    completed.error() != EScriptAwaitableCompletionError::ALREADY_TERMINAL)
                {
                    return false;
                }
            }
            return true;
        }

        class ResumeBatch final
        {
        public:
            ResumeBatch(const ResumeBatch&) = delete;
            ResumeBatch& operator=(const ResumeBatch&) = delete;
            [[nodiscard]] bool next(Result& result) noexcept
            {
                if (remaining_ == 0U)
                    return false;
                const auto record = owner_.resumes_.pop();
                if (!record)
                    return false;
                --remaining_; // Every pop, including a stale one, consumes exactly one budget unit.
                result = owner_.resumeOne(*record);
                return true;
            }
        private:
            friend class ScriptExecution;
            ResumeBatch(ScriptExecution& owner, std::size_t budget) noexcept
                : owner_(owner), region_(owner.instance_owner_), remaining_(owner.stopping_ ? 0U : budget) {}
            ScriptExecution& owner_;
            ScriptInstances::Protection region_;
            std::size_t remaining_{};
        };
        [[nodiscard]] ResumeBatch resumeBatch() noexcept
        {
            return ResumeBatch{*this, limits_.resumes_per_stable_point};
        }

        void shutdown() noexcept;
        void writeStats(ScriptRuntimeStats& result) const noexcept;
    private:
        ScriptInstances& instance_owner_;
        ScriptBindings& binding_owner_;
        ScriptEventWaits& event_owner_;
        ScriptTimers& timer_owner_;
        ScriptCompletionIngress& ingress_;
        ScriptRuntimeLimits limits_;
        ScriptExecutionFailureSink failures_;
        std::vector<ExecutionInstance> execution_instances_;
        struct HookFlight final
        {
            ScriptInstanceId instance;
            ScriptContinuationId continuation;
        };
        std::vector<HookFlight> active_hooks_;
        ContinuationStorage continuations_;
        AwaitableStorage awaitables_;
        ResumeRing resumes_;
        std::size_t instance_cleanup_awaitable_visits_{};
        std::size_t instance_cleanup_continuation_visits_{};
        std::size_t pending_awaitable_releases_{};
        std::size_t result_write_pins_{};
        std::uint64_t event_payload_copy_bytes_{};
        std::uint64_t sync_invocations_{};
        std::uint64_t step_invocations_{};
        std::uint64_t backend_resume_calls_{};
        std::uint64_t suspensions_admitted_{};
        bool stopping_{};
        bool prepared_{};
    };
} // namespace lux::simulation::script::detail
