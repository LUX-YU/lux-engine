#include <lux/engine/simulation/scripting/ScriptSignatureCompatibility.hpp>
#include <lux/engine/simulation/ScriptSystem.hpp>
#include <lux/cxx/container/StableSlotMap.hpp>
#include <lux/engine/simulation/script/ScriptPreparer.hpp>
#include <lux/engine/simulation/script/ExternalCompletionRing.hpp>
#include <lux/engine/simulation/abilities/DelayAbility.hpp>
#include "DelayAbility.ability.generated.hpp"
#include <lux/engine/simulation/scripting/ScriptLifecycle.hpp>

#include <lux/cxx/container/SlotMap.hpp>

#include <entt/signal/sigh.hpp>
#include <entt/container/dense_map.hpp>

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <exception>
#include <limits>

#include <new>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(LUX_SCRIPT_OWNER_AFFINITY_PROBE)
#include <thread>
#endif

namespace lux::simulation::script
{
    namespace
    {
        constexpr auto kInvalidMethodSlot = detail::kInvalidPreparedMethod;

        enum class EPrepareState : std::uint8_t
        {
            CREATED,
            PREPARING,
            ROLLBACK_PENDING,
            PREPARED,
            SHUT_DOWN,
        };

    }

    struct ScriptSystem::State final
    {
        struct ExecutionPort final
        {
            State* owner{};
        };
        ExecutionPort execution_port{this};
        struct DelayProvider;

        struct NextStepWaitTag;
        using NextStepWaitId = lux::cxx::SlotKey<NextStepWaitTag>;

        struct NextStepWait final
        {
            std::uint64_t target_step{};
            lux::script::ScriptAbilityCompletion<void> completion;
            NextStepWaitId previous;
            NextStepWaitId next;
        };

        using NextStepWaitStorage = lux::cxx::SlotMap<NextStepWait, NextStepWaitTag>;

        struct SimulationDelayWait final
        {
            SimulationDuration deadline{};
            std::uint64_t minimum_step{};
            std::uint64_t sequence{};
            lux::script::ScriptAbilityCompletion<void> completion;
        };

        struct SimulationDelayLater final
        {
            [[nodiscard]] bool operator()(
                const SimulationDelayWait& left,
                const SimulationDelayWait& right
            ) const noexcept
            {
                return left.deadline > right.deadline ||
                    (left.deadline == right.deadline && left.sequence > right.sequence);
            }
        };

        struct DelayProvider final
        {
            State* owner{};

            [[nodiscard]] lux::script::ScriptAbilityStartResult nextStep(
                lux::script::ScriptAbilityCompletion<void> completion
            ) noexcept;

            [[nodiscard]] lux::script::ScriptAbilityStartResult seconds(
                double duration,
                lux::script::ScriptAbilityCompletion<void> completion
            ) noexcept;

            [[nodiscard]] lux::script::ScriptAbilityStartResult simulationSeconds(
                double duration,
                lux::script::ScriptAbilityCompletion<void> completion
            ) noexcept;

            [[nodiscard]] lux::script::ScriptAbilityStartResult realSeconds(
                double duration,
                lux::script::ScriptAbilityCompletion<void> completion
            ) noexcept;
        };

        using Handler = detail::ScriptMethodReference;

        using PreparedMethod = detail::ScriptPreparedMethod;
        using RetirementRecord = detail::ScriptInstances::Retirement;

        struct ContinuationTag;
        struct AwaitableTag;
        struct EventWaiterTag;

        struct EventWaiterId final
        {
            std::uint32_t slot{};
            std::uint32_t generation{};

            [[nodiscard]] constexpr bool valid() const noexcept
            {
                return slot != 0U && generation != 0U;
            }

            friend constexpr bool operator==(EventWaiterId, EventWaiterId) noexcept = default;
        };

        struct ExecutionInstance final
        {
            ScriptInstanceId id;
            std::uint32_t mount_slot{};
            std::size_t active_continuations{};
            ScriptContinuationId first_continuation;
            ScriptAwaitableId first_awaitable;
            EventWaiterId first_event_waiter;
            bool admission_revoked{};
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
            std::vector<ResumeRecord> records;
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

            [[nodiscard]] bool push(ResumeRecord record) noexcept
            {
                if (count >= records.size())
                    return false;
                records[(head + count) % records.size()] = record;
                ++count;
                high_water = (std::max)(high_water, count);
                return true;
            }

            [[nodiscard]] std::optional<ResumeRecord> pop() noexcept
            {
                if (count == 0U)
                    return std::nullopt;
                const auto result = records[head];
                head = (head + 1U) % records.size();
                --count;
                return result;
            }

            void clear() noexcept
            {
                head = 0U;
                count = 0U;
            }
        };

        using ExternalCompletionRecord = detail::ExternalCompletionRecord;
        using ExternalCompletionRing = detail::ExternalCompletionRing;

        struct AwaitableRecord final
        {
            ScriptAwaitableId id;
            ScriptInstanceId instance;
            ScriptContinuationId continuation;
            EScriptAwaitableState state{EScriptAwaitableState::PENDING};
            std::optional<PreparedResumeType> result_type;
            ScriptOwnedResumeValue value;
            ScriptStepError error;
            bool resume_enqueued{};
            bool external_completion{};
            bool release_pending{};
            std::uint32_t write_pins{};
            ScriptAwaitableId instance_previous;
            ScriptAwaitableId instance_next;
            EventWaiterId event_waiter;
        };

        enum class EEventWaiterState : std::uint8_t
        {
            ACTIVE,
            CLAIMED,
        };

        struct EventRouteKey final
        {
            std::uint32_t bucket_slot{};
            ecs::Entity target{ecs::NullEntity};

            friend bool operator==(EventRouteKey, EventRouteKey) noexcept = default;
        };

        struct EventRouteKeyHash final
        {
            [[nodiscard]] std::size_t operator()(EventRouteKey key) const noexcept
            {
                const auto bucket_hash = std::hash<std::uint32_t>{}(key.bucket_slot);
                const auto entity_hash = std::hash<std::uint64_t>{}(ecs::entityBits(key.target));
                return bucket_hash ^ (entity_hash + 0x9e3779b9U + (bucket_hash << 6U) + (bucket_hash >> 2U));
            }
        };

        struct EventRouteHead final
        {
            EventWaiterId first;
            EventWaiterId last;
        };

        struct EventWaiterRecord final
        {
            EventWaiterId id;
            ScriptInstanceId instance;
            ScriptAwaitableId awaitable;
            std::uint32_t bucket_slot{};
            ecs::Entity target{ecs::NullEntity};
            std::uint64_t sequence{};
            EEventWaiterState state{EEventWaiterState::ACTIVE};
            EventWaiterId route_previous;
            EventWaiterId route_next;
            EventWaiterId instance_previous;
            EventWaiterId instance_next;
        };

        using ContinuationStorage = lux::cxx::SlotMap<ContinuationRecord, ContinuationTag>;
        using ContinuationKey = typename ContinuationStorage::key_type;
        using AwaitableStorage = lux::cxx::StableSlotMap<AwaitableRecord, AwaitableTag>;
        using AwaitableKey = typename AwaitableStorage::key_type;
        using EventWaiterStorage = lux::cxx::SlotMap<EventWaiterRecord, EventWaiterTag>;
        using EventWaiterKey = typename EventWaiterStorage::key_type;
        using EventRouteIndex = entt::dense_map<EventRouteKey, EventRouteHead, EventRouteKeyHash>;

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

        [[nodiscard]] static constexpr EventWaiterId eventWaiterId(EventWaiterKey key) noexcept
        {
            return {key.index + 1U, key.gen};
        }

        [[nodiscard]] static constexpr EventWaiterKey eventWaiterKey(EventWaiterId id) noexcept
        {
            return id.valid() ? EventWaiterKey{id.slot - 1U, id.generation} : EventWaiterKey::invalid();
        }

        struct AwaitableIngress final
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
                return static_cast<AwaitableIngress*>(context)->complete(instance,
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
                return static_cast<AwaitableIngress*>(context)->completeAbility(
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
                const auto completed = static_cast<AwaitableIngress*>(context)->complete(
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
                return static_cast<AwaitableIngress*>(context)->active(
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
                return static_cast<AwaitableIngress*>(context)->active(instance, awaitable);
            }
        };

        struct SparseMountQueue final
        {
            std::vector<std::uint32_t> values;
            std::vector<std::uint8_t> present;

            void prepare(std::size_t capacity)
            {
                values.clear();
                values.reserve(capacity);
                present.assign(capacity, 0U);
            }

            [[nodiscard]] bool insert(std::uint32_t mount_slot) noexcept
            {
                if (mount_slot >= present.size())
                    return false;
                if (present[mount_slot] != 0U)
                    return true;
                if (values.size() >= values.capacity())
                    return false;

                present[mount_slot] = 1U;
                values.push_back(mount_slot);
                return true;
            }

            void clear() noexcept
            {
                for (const auto mount_slot : values)
                    present[mount_slot] = 0U;
                values.clear();
            }
        };

        const SimulationDescription* simulation{};
        ecs::Registry* registry{};
        const SimulationClock* clock{};
        ScriptRuntimeLimits limits;
        ScriptRealDelayEndpoint real_delay;
        detail::ScriptPreparer preparer;
        detail::ScriptInstances instance_owner;
        detail::ScriptBindings binding_owner;
        std::vector<ScriptSystemFailure> failures;
        std::vector<ExecutionInstance> execution_instances;
        struct HookFlight final
        {
            ScriptInstanceId instance;
            ScriptContinuationId continuation;
        };
        std::vector<HookFlight> active_hooks;
        ContinuationStorage continuations;
        AwaitableStorage awaitables;
        ResumeRing resumes;
        EventWaiterStorage event_waiters;
        EventRouteIndex event_wait_routes;
        std::vector<EventWaiterId> claimed_event_waiters;
        std::shared_ptr<AwaitableIngress> ingress;
        std::vector<std::uint32_t> retirement_queue;
        SparseMountQueue dirty_current;
        SparseMountQueue dirty_processing;
        std::vector<std::uint32_t> lifecycle_candidates;
        std::vector<std::uint32_t> lifecycle_initialized;
        std::vector<RetirementRecord> lifecycle_retirements;
        DelayProvider delay_provider{this};
        std::uint64_t last_stable_step{};
        std::size_t external_admission_frontier{};
        std::size_t external_admission_remaining{};
        bool external_admission_prepared{};
        NextStepWaitStorage next_step_waits;
        NextStepWaitId next_step_first;
        NextStepWaitId next_step_last;
        std::vector<SimulationDelayWait> simulation_delays;
        std::uint64_t delay_sequence{};
        std::uint64_t event_wait_sequence{};
        std::size_t event_waiter_high_water{};
        std::size_t event_waiter_dispatch_visits{};
        std::size_t instance_cleanup_event_waiter_visits{};
        std::size_t instance_cleanup_awaitable_visits{};
        std::size_t instance_cleanup_continuation_visits{};
        std::size_t endpoint_dispatch_depth{};
        std::uint64_t completion_capability_constructions{};
        std::uint64_t event_route_claim_lookups{};
        std::uint64_t event_payload_copy_bytes{};
        std::size_t pending_awaitable_releases{};
        std::size_t result_write_pins{};
        std::size_t active_claimed_waiters{};
        std::uint64_t sync_invocations{};
        std::uint64_t step_invocations{};
        std::uint64_t backend_resume_calls{};
        std::uint64_t suspensions_admitted{};
        std::uint64_t event_occurrences{};

        struct UserInvocationScope final
        {
            explicit UserInvocationScope(State& state) noexcept : protection(state.instance_owner) {}
            detail::ScriptInstances::Protection protection;
        };
        entt::connection constructed;
        entt::connection updated;
        entt::connection destroyed;
        bool stopping{};
        EPrepareState prepare_state{EPrepareState::CREATED};
#if defined(LUX_SCRIPT_OWNER_AFFINITY_PROBE)
        std::thread::id execution_owner;
        std::size_t execution_depth{};
#endif

        [[nodiscard]] bool enterExecutionOwner() noexcept
        {
#if defined(LUX_SCRIPT_OWNER_AFFINITY_PROBE)
            const auto current = std::this_thread::get_id();
            if (execution_owner == std::thread::id{})
                execution_owner = current;
            if (execution_owner != current)
                return false;
            ++execution_depth;
#endif
            return true;
        }

        void leaveExecutionOwner() noexcept
        {
#if defined(LUX_SCRIPT_OWNER_AFFINITY_PROBE)
            if (execution_owner != std::this_thread::get_id() || execution_depth == 0U)
                std::terminate();
            --execution_depth;
#endif
        }

        class ExecutionOwnerScope final
        {
        public:
            explicit ExecutionOwnerScope(State& owner) noexcept
                : owner_(owner), entered_(owner_.enterExecutionOwner())
            {
            }

            ~ExecutionOwnerScope()
            {
                if (entered_)
                    owner_.leaveExecutionOwner();
            }

            [[nodiscard]] explicit operator bool() const noexcept
            {
                return entered_;
            }

        private:
            State& owner_;
            bool entered_{};
        };

        [[nodiscard]] ExecutionInstance* executionRecord(ScriptInstanceId id) noexcept
        {
            if (!id.valid() || id.slot > execution_instances.size())
                return nullptr;
            auto& record = execution_instances[id.slot - 1U];
            return record.id == id ? &record : nullptr;
        }

        [[nodiscard]] ExecutionInstance* findExecutionInstance(ScriptInstanceId id) noexcept
        {
            return instance_owner.valid(id) ? executionRecord(id) : nullptr;
        }

        [[nodiscard]] const ExecutionInstance* findExecutionInstance(ScriptInstanceId id) const noexcept
        {
            if (!instance_owner.valid(id) || id.slot > execution_instances.size())
                return nullptr;
            const auto& record = execution_instances[id.slot - 1U];
            return record.id == id ? &record : nullptr;
        }

        [[nodiscard]] bool validInstance(ScriptInstanceId instance) const noexcept
        {
            return instance_owner.valid(instance);
        }

        [[nodiscard]] lux::script::ScriptAbilityStartResult startNextStep(
            lux::script::ScriptAbilityCompletion<void> completion
        ) noexcept
        {
            if (stopping || !completion.active())
            {
                return lux::cxx::unexpected(lux::script::ScriptAbilityOperationError{
                    static_cast<std::int32_t>(EScriptDelayStatus::STOPPING)
                });
            }
            const auto current = clock->snapshot();
            if (current.step_index == std::numeric_limits<std::uint64_t>::max())
            {
                return lux::cxx::unexpected(lux::script::ScriptAbilityOperationError{
                    static_cast<std::int32_t>(EScriptDelayStatus::DURATION_OVERFLOW)
                });
            }

            if (next_step_waits.size() >= limits.next_step_wait_capacity)
            {
                return lux::cxx::unexpected(lux::script::ScriptAbilityOperationError{
                    static_cast<std::int32_t>(EScriptDelayStatus::CAPACITY_EXCEEDED)
                });
            }

            const auto inserted = next_step_waits.tryEmplace(NextStepWait{
                current.step_index + 1U,
                std::move(completion),
                next_step_last,
                {}
            });
            if (!inserted)
            {
                return lux::cxx::unexpected(lux::script::ScriptAbilityOperationError{
                    static_cast<std::int32_t>(EScriptDelayStatus::ALLOCATION_FAILURE)
                });
            }

            if (auto* previous = next_step_waits.find(next_step_last))
                previous->next = *inserted;
            else
                next_step_first = *inserted;
            next_step_last = *inserted;
            return {};
        }

        [[nodiscard]] bool promoteNextStepWaits() noexcept
        {
            const auto current = clock->snapshot();
            for (;;)
            {
                auto* ready = next_step_waits.find(next_step_first);
                if (ready == nullptr)
                {
                    next_step_first = {};
                    next_step_last = {};
                    return true;
                }
                if (ready->target_step > current.step_index)
                    return true;

                const auto completed = lux::script::detail::ScriptAbilityOwnerCompletionAccess::success(
                    ready->completion
                );
                if (!completed && completed.error() == lux::script::EScriptAbilityCompletionError::BACKPRESSURE)
                    return true;

                const auto ready_id = next_step_first;
                next_step_first = ready->next;
                if (auto* next = next_step_waits.find(next_step_first))
                    next->previous = {};
                else
                    next_step_last = {};
                static_cast<void>(next_step_waits.erase(ready_id));

                if (completed)
                    continue;
                switch (completed.error())
                {
                case lux::script::EScriptAbilityCompletionError::STALE:
                case lux::script::EScriptAbilityCompletionError::STOPPING:
                case lux::script::EScriptAbilityCompletionError::ALREADY_COMPLETED:
                    continue;
                case lux::script::EScriptAbilityCompletionError::BACKPRESSURE:
                    return true;
                case lux::script::EScriptAbilityCompletionError::INVALID_VALUE:
                case lux::script::EScriptAbilityCompletionError::ALLOCATION_FAILURE:
                    return false;
                }
            }
        }

        [[nodiscard]] lux::script::ScriptAbilityStartResult startSimulationDelay(
            double duration,
            lux::script::ScriptAbilityCompletion<void> completion
        ) noexcept
        {
            if (stopping || !completion.active())
            {
                return lux::cxx::unexpected(lux::script::ScriptAbilityOperationError{
                    static_cast<std::int32_t>(EScriptDelayStatus::STOPPING)
                });
            }
            if (!std::isfinite(duration) || duration < 0.0)
            {
                return lux::cxx::unexpected(lux::script::ScriptAbilityOperationError{
                    static_cast<std::int32_t>(EScriptDelayStatus::INVALID_DURATION)
                });
            }

            const long double requested_nanoseconds = static_cast<long double>(duration) * 1'000'000'000.0L;
            const auto maximum = static_cast<long double>(std::numeric_limits<SimulationDuration::rep>::max());
            if (requested_nanoseconds > maximum)
            {
                return lux::cxx::unexpected(lux::script::ScriptAbilityOperationError{
                    static_cast<std::int32_t>(EScriptDelayStatus::DURATION_OVERFLOW)
                });
            }
            const auto duration_count = duration == 0.0
                ? SimulationDuration::rep{}
                : static_cast<SimulationDuration::rep>(std::ceil(requested_nanoseconds));
            const auto current = clock->snapshot();
            const bool is_step_overflow = current.step_index == std::numeric_limits<std::uint64_t>::max();
            const bool is_deadline_overflow = duration_count > 0 &&
                current.elapsed.count() > std::numeric_limits<SimulationDuration::rep>::max() - duration_count;
            if (is_step_overflow || is_deadline_overflow)
            {
                return lux::cxx::unexpected(lux::script::ScriptAbilityOperationError{
                    static_cast<std::int32_t>(EScriptDelayStatus::DURATION_OVERFLOW)
                });
            }
            if (simulation_delays.size() >= limits.simulation_delay_capacity)
            {
                std::erase_if(simulation_delays, [](const SimulationDelayWait& wait) noexcept {
                    return !wait.completion.active();
                });
                std::make_heap(simulation_delays.begin(), simulation_delays.end(), SimulationDelayLater{});
            }
            if (simulation_delays.size() >= limits.simulation_delay_capacity)
            {
                return lux::cxx::unexpected(lux::script::ScriptAbilityOperationError{
                    static_cast<std::int32_t>(EScriptDelayStatus::CAPACITY_EXCEEDED)
                });
            }
            if (delay_sequence == std::numeric_limits<std::uint64_t>::max())
            {
                return lux::cxx::unexpected(lux::script::ScriptAbilityOperationError{
                    static_cast<std::int32_t>(EScriptDelayStatus::DURATION_OVERFLOW)
                });
            }
            try
            {
                simulation_delays.push_back({
                    current.elapsed + SimulationDuration{duration_count},
                    current.step_index + 1U,
                    delay_sequence++,
                    std::move(completion)
                });
                std::push_heap(simulation_delays.begin(), simulation_delays.end(), SimulationDelayLater{});
                return {};
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(lux::script::ScriptAbilityOperationError{
                    static_cast<std::int32_t>(EScriptDelayStatus::ALLOCATION_FAILURE)
                });
            }
        }

        [[nodiscard]] lux::script::ScriptAbilityStartResult startRealDelay(
            double duration,
            lux::script::ScriptAbilityCompletion<void> completion
        ) noexcept
        {
            if (stopping || !completion.active() || !real_delay)
            {
                return lux::cxx::unexpected(lux::script::ScriptAbilityOperationError{
                    static_cast<std::int32_t>(EScriptDelayStatus::STOPPING)
                });
            }
            if (!std::isfinite(duration) || duration < 0.0)
            {
                return lux::cxx::unexpected(lux::script::ScriptAbilityOperationError{
                    static_cast<std::int32_t>(EScriptDelayStatus::INVALID_DURATION)
                });
            }
            if (duration == 0.0)
                return startNextStep(std::move(completion));

            const long double requested_nanoseconds = static_cast<long double>(duration) * 1'000'000'000.0L;
            const auto maximum = static_cast<long double>(std::numeric_limits<std::chrono::nanoseconds::rep>::max());
            if (requested_nanoseconds > maximum)
            {
                return lux::cxx::unexpected(lux::script::ScriptAbilityOperationError{
                    static_cast<std::int32_t>(EScriptDelayStatus::DURATION_OVERFLOW)
                });
            }
            const auto duration_count = static_cast<std::chrono::nanoseconds::rep>(
                std::ceil(requested_nanoseconds)
            );
            return real_delay.invoke(std::chrono::nanoseconds{duration_count}, std::move(completion));
        }

        [[nodiscard]] bool promoteSimulationDelays() noexcept
        {
            const auto current = clock->snapshot();
            for (;;)
            {
                std::optional<SimulationDelayWait> ready;
                {
                    if (simulation_delays.empty())
                        return true;
                    const auto& next = simulation_delays.front();
                    if (next.deadline > current.elapsed || next.minimum_step > current.step_index)
                        return true;
                    std::pop_heap(simulation_delays.begin(), simulation_delays.end(), SimulationDelayLater{});
                    ready.emplace(std::move(simulation_delays.back()));
                    simulation_delays.pop_back();
                }

                auto completed = lux::script::detail::ScriptAbilityOwnerCompletionAccess::success(
                    ready->completion
                );
                if (completed)
                    continue;
                switch (completed.error())
                {
                case lux::script::EScriptAbilityCompletionError::STALE:
                case lux::script::EScriptAbilityCompletionError::STOPPING:
                case lux::script::EScriptAbilityCompletionError::ALREADY_COMPLETED:
                    continue;
                case lux::script::EScriptAbilityCompletionError::BACKPRESSURE:
                {
                    simulation_delays.push_back(std::move(*ready));
                    std::push_heap(simulation_delays.begin(), simulation_delays.end(), SimulationDelayLater{});
                    return true;
                }
                case lux::script::EScriptAbilityCompletionError::INVALID_VALUE:
                case lux::script::EScriptAbilityCompletionError::ALLOCATION_FAILURE:
                    return false;
                }
            }
        }

        [[nodiscard]] lux::cxx::expected<ScriptAwaitableId, EScriptAwaitableCreateError> createAwaitableRecord(
            ScriptInstanceId instance,
            std::optional<PreparedResumeType> result_type,
            bool external_completion = true
        ) noexcept
        {
            auto* owner = findExecutionInstance(instance);
            if (owner == nullptr)
                return lux::cxx::unexpected(EScriptAwaitableCreateError::INVALID_INSTANCE);
            const bool is_invalid_result_type = result_type &&
                (!result_type->valid() ||
                    result_type->size > limits.max_resume_payload_bytes);
            if (is_invalid_result_type)
            {
                return lux::cxx::unexpected(EScriptAwaitableCreateError::INVALID_RESULT_TYPE);
            }
            if (external_completion && result_type &&
                !supportsExternalResumeLayout(result_type->size, result_type->alignment))
            {
                return lux::cxx::unexpected(EScriptAwaitableCreateError::EXTERNAL_RESULT_NOT_TRANSPORTABLE);
            }

            if (stopping)
                return lux::cxx::unexpected(EScriptAwaitableCreateError::STOPPING);
            if (awaitables.size() >= limits.awaitable_capacity)
                return lux::cxx::unexpected(EScriptAwaitableCreateError::CAPACITY_EXCEEDED);
            auto inserted = awaitables.tryEmplacePrepared(AwaitableRecord{
                {},
                instance,
                {},
                EScriptAwaitableState::PENDING,
                std::move(result_type),
                {},
                {},
                false,
                external_completion
            });
            if (!inserted)
                return lux::cxx::unexpected(EScriptAwaitableCreateError::ALLOCATION_FAILURE);
            const auto id = awaitableId(*inserted);
            auto& record = *awaitables.find(*inserted);
            if (!external_completion && record.result_type)
            {
                record.value.type = *record.result_type;
                if (!record.value.bytes.resize(record.result_type->size, record.result_type->alignment))
                {
                    static_cast<void>(awaitables.erase(*inserted));
                    return lux::cxx::unexpected(EScriptAwaitableCreateError::ALLOCATION_FAILURE);
                }
            }
            record.id = id;
            record.instance_next = owner->first_awaitable;
            if (record.instance_next.valid())
            {
                auto* next = awaitables.find(awaitableKey(record.instance_next));
                if (next == nullptr)
                    std::terminate();
                next->instance_previous = id;
            }
            owner->first_awaitable = id;
            if (external_completion)
                ingress->completions.open(id, record.result_type);
            return id;
        }

        [[nodiscard]] lux::cxx::expected<ScriptAwaitableRegistration, EScriptAwaitableCreateError> createAwaitable(
            ScriptInstanceId instance, std::optional<PreparedResumeType> result_type
        ) noexcept
        {
            const auto created = createAwaitableRecord(instance, result_type);
            if (!created) return lux::cxx::unexpected(created.error());
            const auto id = *created;
            ++completion_capability_constructions;
            return ScriptAwaitableRegistration{id,
                                               ScriptAwaitableCompletion{std::static_pointer_cast<void>(ingress),
                                                                         ingress.get(),
                                                                         &AwaitableIngress::completeErased,
                                                                         &AwaitableIngress::activeErased,
                                                                         instance,
                                                                         id,
                                                                         &AwaitableIngress::completeAbilityErased,
                                                                         &AwaitableIngress::failAbilityErased,
                                                                         &AwaitableIngress::activeAbilityErased,
                                                                         &execution_port,
                                                                         &State::completeAbilityOwnerErased,
                                                                         &State::failAbilityOwnerErased}};
        }

        [[nodiscard]] static lux::cxx::expected<ScriptAwaitableRegistration, EScriptAwaitableCreateError>
        createAwaitableErased(void* context,
                              ScriptInstanceId instance,
                              std::optional<PreparedResumeType> result_type) noexcept
        {
            return static_cast<ExecutionPort*>(context)->owner->createAwaitable(instance, std::move(result_type));
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
                const bool is_invalid_value = error.valid() || value.bytes.size() > limits.max_resume_payload_bytes ||
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
            if (stopping)
                return lux::cxx::unexpected(EScriptAwaitableCompletionError::STOPPING);
            auto* record = awaitables.find(awaitableKey(awaitable));
            if (record == nullptr || record->instance != instance)
                return lux::cxx::unexpected(EScriptAwaitableCompletionError::INVALID_ID);
            return finishAwaitableOwner(*record, state, &value, error);
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptAwaitableCompletionError> finishAwaitableOwner(
            AwaitableRecord& record, EScriptAwaitableState state, ScriptOwnedResumeValue* supplied,
            ScriptStepError error
        ) noexcept
        {
            if (stopping) return lux::cxx::unexpected(EScriptAwaitableCompletionError::STOPPING);
            if (record.state != EScriptAwaitableState::PENDING || record.release_pending)
                return lux::cxx::unexpected(EScriptAwaitableCompletionError::ALREADY_TERMINAL);
            if (!validAwaitableOutcome(record, state, supplied ? *supplied : record.value, error))
                return lux::cxx::unexpected(EScriptAwaitableCompletionError::INVALID_VALUE);
            if (record.continuation.valid() && resumes.count >= resumes.records.size())
                return lux::cxx::unexpected(EScriptAwaitableCompletionError::RESUME_QUEUE_FULL);
            record.state = state;
            if (supplied != nullptr) record.value = std::move(*supplied);
            record.error = error;
            if (record.external_completion)
                ingress->completions.close(record.id);
            if (record.continuation.valid())
            {
                static_cast<void>(resumes.push({record.instance, record.continuation, record.id}));
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
            auto& owner = *static_cast<ExecutionPort*>(context)->owner;
            const auto instance = AwaitableIngress::unpackInstance(instance_value);
            const auto awaitable = AwaitableIngress::unpackAwaitable(awaitable_value);
            const auto* record = owner.awaitables.find(awaitableKey(awaitable));
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

            const auto completed = owner.completeAwaitableOwner(
                instance,
                awaitable,
                EScriptAwaitableState::READY,
                std::move(value),
                {}
            );
            return completed
                ? lux::cxx::expected<void, lux::script::EScriptAbilityCompletionError>{}
                : lux::cxx::unexpected(AwaitableIngress::abilityError(completed.error()));
        }

        [[nodiscard]] static lux::cxx::expected<void, lux::script::EScriptAbilityCompletionError>
        failAbilityOwnerErased(
            void* context,
            std::uint64_t instance_value,
            std::uint64_t awaitable_value,
            lux::script::ScriptAbilityOperationError error
        ) noexcept
        {
            auto& owner = *static_cast<ExecutionPort*>(context)->owner;
            const auto completed = owner.completeAwaitableOwner(
                AwaitableIngress::unpackInstance(instance_value),
                AwaitableIngress::unpackAwaitable(awaitable_value),
                EScriptAwaitableState::FAILED,
                {},
                {error.status}
            );
            return completed
                ? lux::cxx::expected<void, lux::script::EScriptAbilityCompletionError>{}
                : lux::cxx::unexpected(AwaitableIngress::abilityError(completed.error()));
        }

        [[nodiscard]] bool drainExternalCompletions() noexcept
        {
            while (ingress->completions.dequeue_position < external_admission_frontier &&
                external_admission_remaining != 0U)
            {
                const auto* external = ingress->completions.front();
                if (external == nullptr)
                    break;
                --external_admission_remaining;
                auto* record = awaitables.find(awaitableKey(external->awaitable));
                const bool is_stale = record == nullptr || record->instance != external->instance;
                if (is_stale)
                {
                    ingress->completions.pop();
                    continue;
                }
                if (record->continuation.valid() && resumes.count >= resumes.records.size())
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
                    ingress->completions.pop();
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
                ingress->completions.pop();
                if (!completed && completed.error() != EScriptAwaitableCompletionError::INVALID_ID &&
                    completed.error() != EScriptAwaitableCompletionError::ALREADY_TERMINAL)
                {
                    return false;
                }
            }
            return true;
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

        void unlinkEventWaiterRoute(EventWaiterRecord& waiter) noexcept
        {
            if (waiter.state != EEventWaiterState::ACTIVE)
                return;
            const EventRouteKey key{waiter.bucket_slot, waiter.target};
            auto route = event_wait_routes.find(key);
            if (route == event_wait_routes.end())
                std::terminate();
            if (waiter.route_previous.valid())
            {
                auto* previous = event_waiters.find(eventWaiterKey(waiter.route_previous));
                if (previous == nullptr)
                    std::terminate();
                previous->route_next = waiter.route_next;
            }
            else
            {
                route->second.first = waiter.route_next;
            }
            if (waiter.route_next.valid())
            {
                auto* next = event_waiters.find(eventWaiterKey(waiter.route_next));
                if (next == nullptr)
                    std::terminate();
                next->route_previous = waiter.route_previous;
            }
            else
            {
                route->second.last = waiter.route_previous;
            }
            waiter.route_previous = {};
            waiter.route_next = {};
            if (!route->second.first.valid())
                event_wait_routes.erase(key);
        }

        void unlinkEventWaiterOwnership(EventWaiterRecord& waiter) noexcept
        {
            auto* owner = findExecutionInstance(waiter.instance);
            if (waiter.instance_previous.valid())
            {
                auto* previous = event_waiters.find(eventWaiterKey(waiter.instance_previous));
                if (previous != nullptr)
                    previous->instance_next = waiter.instance_next;
            }
            else if (owner != nullptr && owner->first_event_waiter == waiter.id)
            {
                owner->first_event_waiter = waiter.instance_next;
            }
            if (waiter.instance_next.valid())
            {
                auto* next = event_waiters.find(eventWaiterKey(waiter.instance_next));
                if (next != nullptr)
                    next->instance_previous = waiter.instance_previous;
            }
            waiter.instance_previous = {};
            waiter.instance_next = {};
        }

        void clearAwaitableEventWaiter(ScriptInstanceId instance,
                                       ScriptAwaitableId awaitable,
                                       EventWaiterId waiter) noexcept
        {
            auto* record = awaitables.find(awaitableKey(awaitable));
            if (record != nullptr && record->instance == instance && record->event_waiter == waiter)
                record->event_waiter = {};
        }

        void eraseEventWaiter(EventWaiterId id, bool clear_awaitable = true) noexcept
        {
            auto* waiter = event_waiters.find(eventWaiterKey(id));
            if (waiter == nullptr)
                return;
            if (waiter->state == EEventWaiterState::ACTIVE)
                unlinkEventWaiterRoute(*waiter);
            else
                --active_claimed_waiters;
            unlinkEventWaiterOwnership(*waiter);
            if (clear_awaitable)
                clearAwaitableEventWaiter(waiter->instance, waiter->awaitable, waiter->id);
            static_cast<void>(event_waiters.erase(eventWaiterKey(id)));
        }

        [[nodiscard]] lux::cxx::expected<ScriptAwaitableId, EScriptEventWaitError> waitEvent(
            ScriptInstanceId instance,
            ScriptEventAdmissionHandle admission
        ) noexcept
        {
            if (stopping || prepare_state != EPrepareState::PREPARED)
                return lux::cxx::unexpected(EScriptEventWaitError::STOPPING);
            auto* owner = findExecutionInstance(instance);
            if (owner == nullptr)
                return lux::cxx::unexpected(EScriptEventWaitError::INVALID_INSTANCE);
            const auto source = instance_owner.eventSource(instance, admission);
            if (!source)
                return lux::cxx::unexpected(source.error());
            const auto endpoint_slot = source->endpoint;
            const auto& endpoint = binding_owner.eventEndpoint(endpoint_slot);
            ecs::Entity target{ecs::NullEntity};
            if (endpoint.route == EEventRoute::ENTITY_TARGETED)
            {
                const auto* entity = std::get_if<EntityScriptScope>(&source->scope);
                if (entity == nullptr || entity->self == ecs::NullEntity || !registry->valid(entity->self))
                    return lux::cxx::unexpected(EScriptEventWaitError::SCOPE_MISMATCH);
                target = entity->self;
            }

            const auto reserved_waiters = event_waiters.size() - active_claimed_waiters + claimed_event_waiters.size();
            if (reserved_waiters >= limits.event_wait_capacity)
                return lux::cxx::unexpected(EScriptEventWaitError::WAITER_CAPACITY_EXCEEDED);
            if (event_wait_sequence == std::numeric_limits<std::uint64_t>::max())
                return lux::cxx::unexpected(EScriptEventWaitError::SEQUENCE_EXHAUSTED);

            auto awaitable = createAwaitableRecord(instance, source->payload, false);
            if (!awaitable)
                return lux::cxx::unexpected(eventWaitError(awaitable.error()));

            const EventRouteKey route_key{endpoint_slot, target};
            bool inserted_route{};
            try
            {
                const auto route = event_wait_routes.try_emplace(route_key, EventRouteHead{});
                inserted_route = route.second;
            }
            catch (const std::bad_alloc&)
            {
                discardAwaitable(instance, *awaitable);
                return lux::cxx::unexpected(EScriptEventWaitError::ALLOCATION_FAILURE);
            }

            auto inserted = event_waiters.tryEmplace(EventWaiterRecord{
                {},
                instance,
                *awaitable,
                endpoint_slot,
                target,
                ++event_wait_sequence,
                EEventWaiterState::ACTIVE,
                {},
                {},
                {},
                {}
            });
            if (!inserted)
            {
                if (inserted_route)
                    event_wait_routes.erase(route_key);
                discardAwaitable(instance, *awaitable);
                return lux::cxx::unexpected(EScriptEventWaitError::ALLOCATION_FAILURE);
            }

            const auto id = eventWaiterId(*inserted);
            auto& waiter = event_waiters[*inserted];
            waiter.id = id;
            auto route = event_wait_routes.find(route_key);
            if (route == event_wait_routes.end())
                std::terminate();
            waiter.route_previous = route->second.last;
            if (waiter.route_previous.valid())
                event_waiters[eventWaiterKey(waiter.route_previous)].route_next = id;
            else
                route->second.first = id;
            route->second.last = id;

            waiter.instance_next = owner->first_event_waiter;
            if (waiter.instance_next.valid())
                event_waiters[eventWaiterKey(waiter.instance_next)].instance_previous = id;
            owner->first_event_waiter = id;
            auto* record = awaitables.find(awaitableKey(*awaitable));
            if (record == nullptr || record->instance != instance)
                std::terminate();
            record->event_waiter = id;
            event_waiter_high_water = (std::max)(event_waiter_high_water, event_waiters.size());
            return *awaitable;
        }

        [[nodiscard]] static lux::cxx::expected<ScriptAwaitableId, EScriptEventWaitError> waitEventErased(
            void* context,
            ScriptInstanceId instance,
            ScriptEventAdmissionHandle admission
        ) noexcept
        {
            return static_cast<ExecutionPort*>(context)->owner->waitEvent(instance, admission);
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptSystemError> attachWaiter(
            ScriptAwaitableId awaitable,
            ScriptInstanceId instance,
            ScriptContinuationId continuation) noexcept
        {
            auto* record = awaitables.find(awaitableKey(awaitable));
            if (record == nullptr || record->instance != instance || record->continuation.valid())
                return lux::cxx::unexpected(EScriptSystemError::INVOCATION_FAILURE);
            if (record->state != EScriptAwaitableState::PENDING &&
                resumes.count >= resumes.records.size())
            {
                return lux::cxx::unexpected(EScriptSystemError::RESUME_QUEUE_FULL);
            }
            record->continuation = continuation;
            if (record->state != EScriptAwaitableState::PENDING)
            {
                static_cast<void>(resumes.push({instance, continuation, awaitable}));
                record->resume_enqueued = true;
            }
            return {};
        }

        struct AwaitableOutcome final
        {
            EScriptAwaitableState state{EScriptAwaitableState::CANCELLED};
            ScriptOwnedResumeValue value;
            ScriptStepError error;
        };

        void unlinkAwaitableOwnership(AwaitableRecord& record) noexcept
        {
            auto* owner = findExecutionInstance(record.instance);
            if (record.instance_previous.valid())
            {
                auto* previous = awaitables.find(awaitableKey(record.instance_previous));
                if (previous != nullptr)
                    previous->instance_next = record.instance_next;
            }
            else if (owner != nullptr && owner->first_awaitable == record.id)
            {
                owner->first_awaitable = record.instance_next;
            }
            if (record.instance_next.valid())
            {
                auto* next = awaitables.find(awaitableKey(record.instance_next));
                if (next != nullptr)
                    next->instance_previous = record.instance_previous;
            }
            record.instance_previous = {};
            record.instance_next = {};
        }

        [[nodiscard]] bool eraseAwaitable(ScriptAwaitableId id) noexcept
        {
            auto* record = awaitables.find(awaitableKey(id));
            if (record == nullptr)
                return false;
            if (record->release_pending) return true;
            unlinkAwaitableOwnership(*record);
            if (record->external_completion)
                ingress->completions.close(id);
            if (record->write_pins != 0U)
            {
                record->state = EScriptAwaitableState::CANCELLED;
                record->release_pending = true;
                ++pending_awaitable_releases;
                return true;
            }
            return awaitables.erase(awaitableKey(id));
        }

        struct ResultWritePin final
        {
            State& owner;
            AwaitableRecord& record;
            ResultWritePin(State& state, AwaitableRecord& value) noexcept : owner(state), record(value)
            {
                ++record.write_pins;
                ++owner.result_write_pins;
            }
            ~ResultWritePin() noexcept
            {
                --record.write_pins;
                --owner.result_write_pins;
                if (record.write_pins == 0U && record.release_pending)
                {
                    const auto key = awaitableKey(record.id);
                    --owner.pending_awaitable_releases;
                    static_cast<void>(owner.awaitables.erase(key));
                }
            }
        };

        [[nodiscard]] std::optional<AwaitableOutcome> takeAwaitable(ResumeRecord resume) noexcept
        {
            auto* record = awaitables.find(awaitableKey(resume.awaitable));
            if (record == nullptr || record->instance != resume.instance ||
                record->continuation != resume.continuation ||
                (record->state != EScriptAwaitableState::READY && record->state != EScriptAwaitableState::FAILED))
            {
                return std::nullopt;
            }
            AwaitableOutcome outcome{record->state, std::move(record->value), record->error};
            static_cast<void>(eraseAwaitable(resume.awaitable));
            return outcome;
        }

        void cancelAwaitables(ScriptInstanceId instance, ScriptAwaitableId first) noexcept
        {
            auto current = first;
            while (current.valid())
            {
                auto* record = awaitables.find(awaitableKey(current));
                if (record == nullptr || record->instance != instance)
                    std::terminate();
                const auto next = record->instance_next;
                static_cast<void>(eraseAwaitable(current));
                ++instance_cleanup_awaitable_visits;
                current = next;
            }
        }

        void cancelEventWaiters(ScriptInstanceId instance, EventWaiterId first) noexcept
        {
            auto current = first;
            while (current.valid())
            {
                const auto* waiter = event_waiters.find(eventWaiterKey(current));
                if (waiter == nullptr || waiter->instance != instance)
                    std::terminate();
                const auto next = waiter->instance_next;
                eraseEventWaiter(current);
                ++instance_cleanup_event_waiter_visits;
                current = next;
            }
        }

        void discardAwaitable(ScriptInstanceId instance, ScriptAwaitableId awaitable) noexcept
        {
            if (!awaitable.valid())
                return;
            EventWaiterId event_waiter;
            auto* record = awaitables.find(awaitableKey(awaitable));
            if (record == nullptr || record->instance != instance)
                return;
            event_waiter = record->event_waiter;
            record->event_waiter = {};
            static_cast<void>(eraseAwaitable(awaitable));
            if (event_waiter.valid())
                eraseEventWaiter(event_waiter, false);
        }

        static void discardAwaitableErased(
            void* context,
            ScriptInstanceId instance,
            ScriptAwaitableId awaitable
        ) noexcept
        {
            static_cast<ExecutionPort*>(context)->owner->discardAwaitable(instance, awaitable);
        }

        [[nodiscard]] std::optional<ResumeRecord> popResume() noexcept
        {
            return resumes.pop();
        }

        void clearActiveHook(const ContinuationRecord& continuation) noexcept
        {
            if (!continuation.hook_single_flight || continuation.method_slot >= active_hooks.size())
                return;
            auto& flight = active_hooks[continuation.method_slot];
            if (flight.instance == continuation.instance && flight.continuation == continuation.id)
                flight = {};
        }

        void destroyContinuation(ScriptContinuationId id) noexcept
        {
            auto* stored = continuations.find(continuationKey(id));
            if (stored == nullptr)
                return;
            const auto continuation = *stored;
            if (auto* instance = findExecutionInstance(continuation.instance); instance != nullptr)
            {
                if (instance->active_continuations == 0U)
                    std::terminate();
                --instance->active_continuations;
                if (!continuation.instance_previous.valid() && instance->first_continuation == id)
                    instance->first_continuation = continuation.instance_next;
            }
            if (continuation.instance_previous.valid())
            {
                auto* previous = continuations.find(continuationKey(continuation.instance_previous));
                if (previous != nullptr)
                    previous->instance_next = continuation.instance_next;
            }
            if (continuation.instance_next.valid())
            {
                auto* next = continuations.find(continuationKey(continuation.instance_next));
                if (next != nullptr)
                    next->instance_previous = continuation.instance_previous;
            }
            clearActiveHook(continuation);
            static_cast<void>(continuations.erase(continuationKey(id)));
            UserInvocationScope cleanup(*this);
            continuation.backend.destroy(continuation.backend.state);
        }

        void destroyContinuations(ScriptInstanceId instance, ScriptContinuationId first) noexcept
        {
            auto current = first;
            while (current.valid())
            {
                const auto* record = continuations.find(continuationKey(current));
                if (record == nullptr || record->instance != instance)
                    std::terminate();
                const auto next = record->instance_next;
                destroyContinuation(current);
                ++instance_cleanup_continuation_visits;
                current = next;
            }
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
            const auto first_event_waiter = record->first_event_waiter;
            UserInvocationScope cleanup(*this);
            cancelEventWaiters(instance, first_event_waiter);
            cancelAwaitables(instance, first_awaitable);
        }

        void invalidateInstance(std::uint32_t slot) noexcept
        {
            const auto retired = instance_owner.view(slot).retiring_instance;
            invalidateAdmission(retired);
            auto* record = executionRecord(retired);
            if (record == nullptr)
                return;
            const auto first = record->first_continuation;
            *record = {}; // Claim execution teardown before entering a continuation destructor.
            UserInvocationScope cleanup(*this);
            destroyContinuations(retired, first);
        }

        void faultInvocation(std::uint32_t slot, lux::script::ScriptSymbolId symbol,
            EScriptSystemError error, std::int32_t status = 0) noexcept
        {
            invalidateAdmission(instance_owner.fault(slot));
            binding_owner.withdraw(slot);
            queueRetirement(slot);
            recordFailure(error, slot, symbol, status);
        }

        [[nodiscard]] bool beginSuspension(std::uint32_t mount_slot, std::uint32_t method_slot,
                                           const PreparedMethod& method,
                                           ScriptBackendContinuation backend_continuation,
                                           ScriptStepResult result,
                                           bool hook_single_flight) noexcept
        {
            const auto mount = instance_owner.view(mount_slot);
            if (!result.valid() || result.state != EScriptStepState::SUSPENDED || !backend_continuation)
            {
                if (backend_continuation)
                    backend_continuation.destroy(backend_continuation.state);
                discardAwaitable(mount.instance, result.waiting_on);
                faultInvocation(mount_slot, method.symbol, EScriptSystemError::INVOCATION_FAILURE);
                return false;
            }
            auto* instance = findExecutionInstance(mount.instance);
            if (instance == nullptr)
            {
                backend_continuation.destroy(backend_continuation.state);
                discardAwaitable(mount.instance, result.waiting_on);
                faultInvocation(mount_slot, method.symbol, EScriptSystemError::INVOCATION_FAILURE);
                return false;
            }
            if (instance->active_continuations >= limits.continuation_capacity_per_instance)
            {
                backend_continuation.destroy(backend_continuation.state);
                discardAwaitable(mount.instance, result.waiting_on);
                faultInvocation(
                    mount_slot,
                    method.symbol,
                    EScriptSystemError::INSTANCE_CONTINUATION_CAPACITY_EXCEEDED
                );
                return false;
            }
            if (continuations.size() >= limits.continuation_capacity)
            {
                backend_continuation.destroy(backend_continuation.state);
                discardAwaitable(mount.instance, result.waiting_on);
                faultInvocation(mount_slot, method.symbol, EScriptSystemError::CONTINUATION_CAPACITY_EXCEEDED);
                return false;
            }
            auto inserted = continuations.tryEmplace(
                ContinuationRecord{{},
                                   mount.instance,
                                   backend_continuation,
                                   result.waiting_on,
                                   method_slot,
                                   hook_single_flight});
            if (!inserted)
            {
                backend_continuation.destroy(backend_continuation.state);
                discardAwaitable(mount.instance, result.waiting_on);
                faultInvocation(mount_slot, method.symbol, EScriptSystemError::ALLOCATION_FAILURE);
                return false;
            }
            const auto id = continuationId(*inserted);
            auto& stored = continuations[*inserted];
            stored.id = id;
            stored.instance_next = instance->first_continuation;
            if (stored.instance_next.valid())
            {
                auto* next = continuations.find(continuationKey(stored.instance_next));
                if (next == nullptr)
                    std::terminate();
                next->instance_previous = id;
            }
            instance->first_continuation = id;
            ++instance->active_continuations;
            auto attached = attachWaiter(result.waiting_on, mount.instance, id);
            if (!attached)
            {
                destroyContinuation(id);
                discardAwaitable(mount.instance, result.waiting_on);
                faultInvocation(mount_slot, method.symbol, attached.error());
                return false;
            }
            if (hook_single_flight)
                active_hooks[method_slot] = {mount.instance, id};
            ++suspensions_admitted;
            return true;
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptSystemError> buildLayout() noexcept
        {
            try
            {
                const auto count = instance_owner.capacity();
                dirty_current.prepare(count);
                dirty_processing.prepare(count);
                retirement_queue.reserve(count);
                lifecycle_candidates.reserve(count);
                lifecycle_initialized.reserve(count);
                lifecycle_retirements.reserve(count);
                return {};
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(EScriptSystemError::ALLOCATION_FAILURE);
            }
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptSystemError>
        acceptMounts(std::span<const ScriptRuntimeMount> inputs) noexcept
        {
            if (inputs.empty())
                return {};
            auto instance_ticket = instance_owner.reserveBatch(inputs, binding_owner,
                prepare_state == EPrepareState::CREATED);
            if (!instance_ticket)
                return lux::cxx::unexpected(instance_ticket.error());
            auto binding_ticket = binding_owner.reserveBatch(inputs, instance_ticket->placements());
            if (!binding_ticket)
                return lux::cxx::unexpected(binding_ticket.error());
            const auto placements = instance_ticket->placements();
            binding_owner.commitBatch(std::move(*binding_ticket));
            instance_owner.commitBatch(std::move(*instance_ticket), binding_owner);
            if (prepare_state == EPrepareState::PREPARED)
                for (const auto placement : placements)
                    queueDirty(placement.slot);
            return {};
        }

        void recordFailure(EScriptSystemError error, std::uint32_t slot,
            lux::script::ScriptSymbolId symbol = lux::script::InvalidScriptSymbolId, std::int32_t status = 0) noexcept
        {
            instance_owner.recordError(slot, error);
            if (failures.size() < failures.capacity())
                failures.push_back({error, instance_owner.view(slot).id, symbol, status});
        }

        void queueRetirement(std::uint32_t slot) noexcept
        {
            if (!instance_owner.queueRetirement(slot))
                return;
            if (retirement_queue.size() == retirement_queue.capacity())
                std::terminate();
            retirement_queue.push_back(slot);
        }

        void invoke(Handler handler, lux_script_call_frame& frame, bool hook_invocation) noexcept
        {
            if (stopping)
                return;
            auto access = instance_owner.invokeAccess(handler);
            if (!access)
                return;
            const auto& method = access->method();
            const auto flight = active_hooks[handler.method_slot];
            if (hook_invocation && flight.instance == handler.instance && flight.continuation.valid() &&
                continuations.find(continuationKey(flight.continuation)) != nullptr)
            {
                return;
            }
            if (method.backend.resumable)
            {
                ScriptBackendContinuation continuation;
                ScriptStepContext context{
                    handler.instance,
                    &execution_port,
                    &State::createAwaitableErased,
                    &State::discardAwaitableErased,
                    &State::waitEventErased
                };
                const auto result = [&]() noexcept {
                    UserInvocationScope scope(*this);
                    ++step_invocations;
                    return method.backend.resumable.invoke(
                        method.backend.resumable.context, frame, context, continuation);
                }();
                if (!access->current() || stopping)
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
                        handler.mount_slot, handler.method_slot, method, continuation, result, hook_invocation
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

            frame.user_context = method.backend.synchronous.context;
            const auto status = [&]() noexcept {
                UserInvocationScope scope(*this);
                ++sync_invocations;
                return method.backend.synchronous.invoke(&frame);
            }();
            if (status == 0)
                return;
            faultInvocation(handler.mount_slot, method.symbol, EScriptSystemError::INVOCATION_FAILURE, status);
        }

        struct BindingPort final
        {
            State* owner{};
        };
        BindingPort binding_port{this};

        static void invokeBinding(void* context, Handler handler, lux_script_call_frame& frame, bool hook) noexcept
        {
            static_cast<BindingPort*>(context)->owner->invoke(handler, frame, hook);
        }

        static void invokeHookLane(void* context, std::uint32_t bucket, lux_script_call_frame& frame) noexcept
        {
            auto& owner = *static_cast<BindingPort*>(context)->owner;
            ExecutionOwnerScope execution{owner};
            if (!execution)
                return;
            ++owner.endpoint_dispatch_depth;
            owner.binding_owner.visitHook(bucket, frame);
            --owner.endpoint_dispatch_depth;
        }

        void claimEventWaiters(std::uint32_t bucket, ecs::Entity target, std::uint64_t cutoff) noexcept
        {
            ++event_route_claim_lookups;
            auto route = event_wait_routes.find(EventRouteKey{bucket, target});
            if (route == event_wait_routes.end())
                return;

            auto current = route->second.first;
            while (current.valid())
            {
                auto* waiter = event_waiters.find(eventWaiterKey(current));
                if (waiter == nullptr || waiter->state != EEventWaiterState::ACTIVE)
                    std::terminate();
                ++event_waiter_dispatch_visits;
                if (waiter->sequence > cutoff)
                    break;
                const auto next = waiter->route_next;
                waiter->route_previous = {};
                waiter->route_next = {};
                waiter->state = EEventWaiterState::CLAIMED;
                ++active_claimed_waiters;
                if (claimed_event_waiters.size() >= claimed_event_waiters.capacity())
                    std::terminate();
                claimed_event_waiters.push_back(current);
                current = next;
            }
            if (current.valid())
            {
                route->second.first = current;
                event_waiters[eventWaiterKey(current)].route_previous = {};
            }
            else
                event_wait_routes.erase(route);
        }

        void failEventWaiter(ScriptInstanceId instance, EScriptSystemError error) noexcept
        {
            const auto* owner = findExecutionInstance(instance);
            if (owner != nullptr && instance_owner.active(instance))
                faultInvocation(owner->mount_slot, lux::script::InvalidScriptSymbolId, error);
        }

        void completeClaimedEventWaiter(EventWaiterId id, lux_script_call_frame& frame) noexcept
        {
            auto* waiter = event_waiters.find(eventWaiterKey(id));
            if (waiter == nullptr || waiter->state != EEventWaiterState::CLAIMED)
                return;

            const auto instance = waiter->instance;
            const auto awaitable = waiter->awaitable;
            const bool is_live = instance_owner.active(instance);
            if (!is_live)
            {
                eraseEventWaiter(id);
                discardAwaitable(instance, awaitable);
                return;
            }

            const auto& endpoint = binding_owner.eventEndpoint(waiter->bucket_slot);
            auto* record = awaitables.find(awaitableKey(awaitable));
            if (record == nullptr || record->instance != instance || record->release_pending)
            {
                eraseEventWaiter(id);
                return;
            }
            ResultWritePin pin(*this, *record);
            const bool is_invalid_frame = frame.arg_count != 1U || frame.args == nullptr;
            bool copied{};
            {
                UserInvocationScope invocation(*this);
                copied = !is_invalid_frame && endpoint.payload_projection.copy(
                    endpoint.context, frame.args[0], record->value.bytes.span());
            }
            const bool still_live = !stopping && !record->release_pending && instance_owner.active(instance);
            if (!copied || !still_live)
            {
                eraseEventWaiter(id);
                discardAwaitable(instance, awaitable);
                if (still_live) failEventWaiter(instance, EScriptSystemError::INVOCATION_FAILURE);
                return;
            }
            event_payload_copy_bytes += record->value.bytes.size();
            eraseEventWaiter(id);
            const auto completed = finishAwaitableOwner(*record, EScriptAwaitableState::READY, nullptr, {});
            if (completed)
                return;

            discardAwaitable(instance, awaitable);
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

        static void dispatchEvent(
            void* context, std::uint32_t bucket, ecs::Entity entity, lux_script_call_frame& frame
        ) noexcept
        {
            auto& owner = *static_cast<BindingPort*>(context)->owner;
            ExecutionOwnerScope execution{owner};
            if (!execution)
                return;
            const auto claimed_begin = owner.claimed_event_waiters.size();
            ++owner.event_occurrences;
            const auto cutoff = owner.event_wait_sequence;
            ++owner.endpoint_dispatch_depth;
            const auto& endpoint = owner.binding_owner.eventEndpoint(bucket);
            const auto target = endpoint.route == EEventRoute::SIMULATION_BROADCAST ? ecs::NullEntity : entity;
            owner.claimEventWaiters(bucket, target, cutoff);
            const auto claimed_end = owner.claimed_event_waiters.size();
            owner.binding_owner.visitEvent(bucket, entity, frame);
            for (std::size_t index{claimed_begin}; index < claimed_end; ++index)
                owner.completeClaimedEventWaiter(owner.claimed_event_waiters[index], frame);
            owner.claimed_event_waiters.resize(claimed_begin);
            --owner.endpoint_dispatch_depth;
        }

        [[nodiscard]] bool ownsAttachment(std::uint32_t slot, ecs::Entity entity) const noexcept
        {
            return instance_owner.ownsAttachment(slot, entity);
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptSystemError> beginPlayMount(std::uint32_t slot) noexcept
        {
            const auto result = instance_owner.beginPlay(slot);
            if (result)
                return {};
            const auto error = result.error();
            recordFailure(error.error, slot, error.symbol, error.status);
            return lux::cxx::unexpected(error.error);
        }

        [[nodiscard]] RetirementRecord beginRetirement(std::uint32_t slot, EScriptEndPlayReason reason,
            EScriptMountState final_state, bool remove_attachment) noexcept
        {
            auto retirement = instance_owner.claimRetirement(slot, reason, final_state);
            binding_owner.withdraw(slot);
            if (remove_attachment)
                instance_owner.removeAttachment(slot);
            invalidateInstance(slot);
            return retirement;
        }

        void finishRetirement(const RetirementRecord& retirement) noexcept
        {
            const auto ended = instance_owner.endPlay(retirement);
            if (!ended)
            {
                const auto error = ended.error();
                recordFailure(error.error, retirement.slot(), error.symbol, error.status);
            }
            instance_owner.finishRetirement(retirement);
        }

        void releaseMount(std::uint32_t slot, EScriptMountState final_state, bool remove_attachment,
            EScriptEndPlayReason reason = EScriptEndPlayReason::OBJECT_UNMATERIALIZED) noexcept
        {
            finishRetirement(beginRetirement(slot, reason, final_state, remove_attachment));
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptSystemError> initializeMount(std::uint32_t slot) noexcept
        {
            const auto result = preparer.prepareMount(instance_owner, slot, binding_owner, *simulation, limits);
            if (!result)
                return result;
            const auto mount = instance_owner.view(slot);
            if (mount.state == EScriptMountState::INITIALIZED)
                execution_instances[mount.instance.slot - 1U] = {mount.instance, slot};
            return {};
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptSystemError> publishMount(std::uint32_t slot) noexcept
        {
            const auto mount = instance_owner.view(slot);
            if (mount.state != EScriptMountState::INITIALIZED || !mount.gameplay_lifetime_started)
                return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);
            const auto bound = binding_owner.publish(slot, mount.instance, mount.entity);
            if (!bound)
                return bound;
            const auto projected = instance_owner.projectAttachment(slot);
            if (!projected)
            {
                binding_owner.withdraw(slot);
                return projected;
            }
            return {};
        }

        void activateMount(std::uint32_t slot) noexcept { instance_owner.activate(slot); }

        void queueDirty(std::uint32_t mount_slot) noexcept
        {
            if (!dirty_current.insert(mount_slot))
                std::terminate();
        }

        void handleAttachmentSignal(ecs::Registry& source, ecs::Entity entity, bool destroying) noexcept
        {
            ExecutionOwnerScope execution{*this};
            if (!execution)
                return;
            if (&source != registry)
                return;
            const auto slot = instance_owner.observeAttachment(entity, destroying);
            if (!slot)
                return;
            const auto mount = instance_owner.view(*slot);
            if (destroying && mount.state == EScriptMountState::RETIRING)
            {
                invalidateAdmission(mount.retiring_instance);
                binding_owner.withdraw(*slot);
            }
            queueDirty(*slot);
        }

        void onAttachmentConstructed(ecs::Registry& source, ecs::Entity entity) noexcept
        {
            handleAttachmentSignal(source, entity, false);
        }

        void onAttachmentUpdated(ecs::Registry& source, ecs::Entity entity) noexcept
        {
            handleAttachmentSignal(source, entity, false);
        }

        void onAttachmentDestroyed(ecs::Registry& source, ecs::Entity entity) noexcept
        {
            handleAttachmentSignal(source, entity, true);
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptSystemError> connectEndpoints() noexcept
        {
            return binding_owner.connect();
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptSystemError> disconnectEndpoints() noexcept
        {
            return binding_owner.disconnect();
        }

        void releaseSignals() noexcept
        {
            constructed.release();
            updated.release();
            destroyed.release();
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptSystemError> resumeOne(ResumeRecord resume) noexcept
        {
            if (stopping)
                return {};
            auto* instance = findExecutionInstance(resume.instance);
            auto* continuation = continuations.find(continuationKey(resume.continuation));
            if (instance == nullptr || continuation == nullptr || continuation->instance != resume.instance ||
                continuation->waiting_on != resume.awaitable)
            {
                return {};
            }
            auto outcome = takeAwaitable(resume);
            if (!outcome)
                return {};
            auto access = instance_owner.resumeAccess(resume.instance);
            if (!access)
            {
                destroyContinuation(resume.continuation);
                return {};
            }

            continuation->waiting_on = {};
            ScriptResumePacket packet{resume.awaitable, outcome->state, std::addressof(outcome->value), outcome->error};
            ScriptStepContext context{
                resume.instance,
                &execution_port,
                &State::createAwaitableErased,
                &State::discardAwaitableErased,
                &State::waitEventErased
            };
            const auto result = [&]() noexcept {
                UserInvocationScope scope(*this);
                ++backend_resume_calls;
                return continuation->backend.resume(continuation->backend.state, context, packet);
            }();

            continuation = continuations.find(continuationKey(resume.continuation));
            instance = findExecutionInstance(resume.instance);
            if (continuation == nullptr || instance == nullptr)
                return {};
            if (!access->current())
            {
                destroyContinuation(resume.continuation);
                return {};
            }
            const auto slot = instance->mount_slot;
            const auto symbol = instance_owner.methodSymbol(continuation->method_slot);
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
                    ++suspensions_admitted;
                    return {};
                }
                const auto error = attached.error();
                destroyContinuation(resume.continuation);
                discardAwaitable(resume.instance, result.waiting_on);
                faultInvocation(slot, symbol, error);
                return lux::cxx::unexpected(error);
            }

            const auto status = result.error.status;
            destroyContinuation(resume.continuation);
            faultInvocation(slot, symbol, EScriptSystemError::INVOCATION_FAILURE, status);
            return lux::cxx::unexpected(EScriptSystemError::INVOCATION_FAILURE);
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptSystemError> drainResumes() noexcept
        {
            std::optional<EScriptSystemError> first_error;
            for (std::size_t count{}; count < limits.resumes_per_stable_point; ++count)
            {
                auto resume = popResume();
                if (!resume)
                    break;
                auto resumed = resumeOne(*resume);
                if (!resumed && !first_error)
                    first_error = resumed.error();
                if (!drainExternalCompletions() && !first_error)
                    first_error = EScriptSystemError::INVOCATION_FAILURE;
            }
            return first_error ? lux::cxx::expected<void, EScriptSystemError>(lux::cxx::unexpected(*first_error))
                               : lux::cxx::expected<void, EScriptSystemError>{};
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptSystemError> rollbackPrepare() noexcept
        {
            const auto disconnected = disconnectEndpoints();
            if (!disconnected)
            {
                prepare_state = EPrepareState::ROLLBACK_PENDING;
                return disconnected;
            }

            releaseSignals();
            for (std::size_t index{instance_owner.capacity()}; index > 0U; --index)
            {
                releaseMount(
                    static_cast<std::uint32_t>(index - 1U),
                    EScriptMountState::INACTIVE,
                    true,
                    EScriptEndPlayReason::RUNTIME_STOPPED
                );
            }
            instance_owner.restorePendingAfterRollback();
            dirty_current.clear();
            dirty_processing.clear();
            retirement_queue.clear();
            prepare_state = EPrepareState::CREATED;
            return {};
        }
    };

    lux::script::ScriptAbilityStartResult ScriptSystem::State::DelayProvider::nextStep(
        lux::script::ScriptAbilityCompletion<void> completion
    ) noexcept
    {
        return owner->startNextStep(std::move(completion));
    }

    lux::script::ScriptAbilityStartResult ScriptSystem::State::DelayProvider::seconds(
        double duration,
        lux::script::ScriptAbilityCompletion<void> completion
    ) noexcept
    {
        return owner->startSimulationDelay(duration, std::move(completion));
    }

    lux::script::ScriptAbilityStartResult ScriptSystem::State::DelayProvider::simulationSeconds(
        double duration,
        lux::script::ScriptAbilityCompletion<void> completion
    ) noexcept
    {
        return owner->startSimulationDelay(duration, std::move(completion));
    }

    lux::script::ScriptAbilityStartResult ScriptSystem::State::DelayProvider::realSeconds(
        double duration,
        lux::script::ScriptAbilityCompletion<void> completion
    ) noexcept
    {
        return owner->startRealDelay(duration, std::move(completion));
    }

    lux::cxx::expected<ScriptRuntimeCapacityPlan, EScriptSystemError>
    planScriptRuntimeCapacity(std::span<const ScriptRuntimeMount> mounts) noexcept
    {
        try
        {
            ScriptRuntimeCapacityPlan result;
            result.mount_capacity = mounts.size();
            result.enabled_mount_capacity = mounts.size();
            for (const auto& mount : mounts)
            {
                if (mount.bindings.size() > std::numeric_limits<std::size_t>::max() - result.binding_capacity)
                    return lux::cxx::unexpected(EScriptSystemError::CAPACITY_EXCEEDED);
                result.binding_capacity += mount.bindings.size();
                for (const auto& binding : mount.bindings)
                {
                    const auto existing = std::find_if(result.endpoint_capacities.begin(),
                        result.endpoint_capacities.end(),
                        [&](const auto& endpoint) noexcept { return endpoint.target == binding.target; });
                    if (existing == result.endpoint_capacities.end())
                        result.endpoint_capacities.push_back({binding.target, 1U});
                    else
                        ++existing->handler_capacity;
                }
            }
            if (mounts.size() > (std::numeric_limits<std::size_t>::max() - result.binding_capacity) / 2U)
                return lux::cxx::unexpected(EScriptSystemError::CAPACITY_EXCEEDED);
            result.method_capacity = result.binding_capacity + mounts.size() * 2U;
            return result;
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(EScriptSystemError::ALLOCATION_FAILURE);
        }
    }

    lux::cxx::expected<ScriptSystem, EScriptSystemError> ScriptSystem::create(
        const SimulationDescription& simulation,
        const ScriptRuntimeCapacityPlan& capacity,
        std::span<const ScriptRuntimeMount> mounts,
        ecs::Registry& registry,
        const SimulationClock& clock,
        ScriptRuntimeLimits limits,
        ScriptArtifactResolver artifacts,
        std::span<const ScriptApiCapabilityPublication> capabilities,
        std::span<const ScriptBackendDescriptor> backends,
        std::span<const ScriptHookEndpointDescriptor> hooks,
        std::span<const ScriptEventEndpointDescriptor> events,
        ScriptHostApi host,
        ScriptRealDelayEndpoint real_delay) noexcept
    {
        const bool invalid_limits = limits.failure_capacity == 0U || limits.instance_capacity == 0U ||
                                    limits.continuation_capacity == 0U ||
                                    limits.continuation_capacity_per_instance == 0U ||
                                    limits.continuation_capacity_per_instance > limits.continuation_capacity ||
                                    limits.awaitable_capacity == 0U ||
                                    limits.resume_queue_capacity == 0U || limits.max_resume_payload_bytes == 0U ||
                                    limits.resumes_per_stable_point == 0U || limits.next_step_wait_capacity == 0U ||
                                    limits.simulation_delay_capacity == 0U || limits.event_wait_capacity == 0U ||
                                    limits.external_completion_capacity == 0U;
        if (invalid_limits || artifacts.resolve == nullptr)
            return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);

        const bool invalid_capacity = capacity.enabled_mount_capacity > capacity.mount_capacity ||
            capacity.enabled_mount_capacity > limits.instance_capacity ||
            capacity.mount_capacity >= std::numeric_limits<std::uint32_t>::max() ||
            capacity.method_capacity >= std::numeric_limits<std::uint32_t>::max() ||
            capacity.binding_capacity >= std::numeric_limits<std::uint32_t>::max() ||
            capacity.mount_capacity > (std::numeric_limits<std::size_t>::max() - capacity.binding_capacity) / 2U;
        if (invalid_capacity)
            return lux::cxx::unexpected(EScriptSystemError::CAPACITY_EXCEEDED);
        if (capacity.method_capacity != capacity.binding_capacity + 2U * capacity.mount_capacity)
            return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);
        std::size_t planned_handlers{};
        for (const auto& endpoint : capacity.endpoint_capacities)
        {
            if (endpoint.handler_capacity > capacity.binding_capacity - planned_handlers)
                return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);
            planned_handlers += endpoint.handler_capacity;
        }
        if (planned_handlers != capacity.binding_capacity)
            return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);

        try
        {
            auto state = std::make_unique<State>();
            state->simulation = std::addressof(simulation);
            state->registry = std::addressof(registry);
            state->clock = std::addressof(clock);
            state->limits = limits;
            state->delay_provider.owner = state.get();
            state->next_step_waits.reserve(limits.next_step_wait_capacity);
            state->simulation_delays.reserve(limits.simulation_delay_capacity);

            const auto delay_binding = lux::script::bindScriptAbility<DelayAbility>(state->delay_provider);
            const auto delay_publication = publishScriptAbility(delay_binding);
            const auto catalog = state->preparer.prepareCatalog(artifacts, backends, capabilities, delay_publication);
            if (!catalog)
                return lux::cxx::unexpected(catalog.error());
            const auto instance_layout = state->instance_owner.prepare(
                capacity, limits.instance_capacity, registry, host
            );
            if (!instance_layout)
                return lux::cxx::unexpected(instance_layout.error());
            state->real_delay = real_delay;
            const auto binding_layout = state->binding_owner.prepare(simulation, capacity, hooks, events,
                {&state->binding_port, &State::invokeHookLane, &State::dispatchEvent, &State::invokeBinding},
                limits.max_resume_payload_bytes);
            if (!binding_layout)
                return lux::cxx::unexpected(binding_layout.error());
            state->failures.reserve(limits.failure_capacity);
            state->execution_instances.resize(state->instance_owner.identityCapacity());
            state->active_hooks.resize(capacity.method_capacity);
            state->continuations.reserve(limits.continuation_capacity);
            state->event_waiters.reserve(limits.event_wait_capacity);
            state->event_wait_routes.reserve(limits.event_wait_capacity);
            state->claimed_event_waiters.reserve(limits.event_wait_capacity);
            state->awaitables.reserve(limits.awaitable_capacity);
            state->resumes.prepare(limits.resume_queue_capacity);
            state->ingress = std::make_shared<State::AwaitableIngress>();
            // Stable slots are page-rounded; tickets are indexed by physical SlotKey, not by the
            // configured concurrent-admission limit. The logical limit remains checked on admission.
            state->ingress->completions.prepare(limits.external_completion_capacity, state->awaitables.capacity());
            const auto layout = state->buildLayout();
            if (!layout)
                return lux::cxx::unexpected(layout.error());
            const auto accepted = state->acceptMounts(mounts);
            if (!accepted)
                return lux::cxx::unexpected(accepted.error());
            return ScriptSystem(std::move(state));
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(EScriptSystemError::ALLOCATION_FAILURE);
        }
    }

    ScriptSystem::ScriptSystem(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

    ScriptSystem::ScriptSystem(ScriptSystem&&) noexcept = default;
    ScriptSystem& ScriptSystem::operator=(ScriptSystem&&) noexcept = default;

    ScriptSystem::~ScriptSystem() noexcept
    {
        if (state_ && state_->prepare_state != EPrepareState::SHUT_DOWN && !shutdown())
            std::terminate();
    }

    lux::cxx::expected<void, EScriptSystemError> ScriptSystem::prepare() noexcept
    {
        if (!state_ || state_->prepare_state == EPrepareState::SHUT_DOWN)
            return lux::cxx::unexpected(EScriptSystemError::SHUT_DOWN);
        State::ExecutionOwnerScope execution{*state_};
        if (!execution)
            return lux::cxx::unexpected(EScriptSystemError::ENDPOINT_BUSY);
        if (state_->stopping)
            return lux::cxx::unexpected(EScriptSystemError::ENDPOINT_BUSY);
        if (state_->prepare_state == EPrepareState::PREPARED)
            return {};
        if (state_->prepare_state == EPrepareState::ROLLBACK_PENDING)
            return lux::cxx::unexpected(EScriptSystemError::ENDPOINT_BUSY);
        if (state_->prepare_state == EPrepareState::PREPARING)
            return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);

        state_->prepare_state = EPrepareState::PREPARING;

        state_->constructed = state_->registry->on_construct<detail::ScriptAttachment>()
                                  .template connect<&State::onAttachmentConstructed>(*state_);
        state_->updated =
            state_->registry->on_update<detail::ScriptAttachment>().template connect<&State::onAttachmentUpdated>(
                *state_);
        state_->destroyed =
            state_->registry->on_destroy<detail::ScriptAttachment>().template connect<&State::onAttachmentDestroyed>(
                *state_);

        state_->lifecycle_initialized.clear();
        for (std::size_t mount_slot{}; mount_slot < state_->instance_owner.capacity(); ++mount_slot)
        {
            auto initialized = state_->initializeMount(static_cast<std::uint32_t>(mount_slot));
            if (!initialized)
            {
                const auto rolled_back = state_->rollbackPrepare();
                return rolled_back ? initialized : rolled_back;
            }
            if (state_->instance_owner.view(mount_slot).state == EScriptMountState::INITIALIZED)
                state_->lifecycle_initialized.push_back(static_cast<std::uint32_t>(mount_slot));
        }

        for (const auto mount_slot : state_->lifecycle_initialized)
        {
            const auto begun = state_->beginPlayMount(mount_slot);
            if (!begun)
            {
                state_->releaseMount(mount_slot, EScriptMountState::FAULTED, true);
                const auto rolled_back = state_->rollbackPrepare();
                return rolled_back ? begun : rolled_back;
            }
        }

        for (const auto mount_slot : state_->lifecycle_initialized)
        {
            if (state_->instance_owner.view(mount_slot).state != EScriptMountState::INITIALIZED)
                continue;
            const auto published = state_->publishMount(mount_slot);
            if (!published)
            {
                state_->releaseMount(
                    mount_slot,
                    EScriptMountState::FAULTED,
                    true,
                    EScriptEndPlayReason::FAULTED
                );
                const auto rolled_back = state_->rollbackPrepare();
                return rolled_back ? published : rolled_back;
            }
        }
        for (const auto mount_slot : state_->lifecycle_initialized)
        {
            if (state_->instance_owner.view(mount_slot).state == EScriptMountState::INITIALIZED)
                state_->activateMount(mount_slot);
        }

        const auto connected = state_->connectEndpoints();
        if (!connected)
        {
            const auto rolled_back = state_->rollbackPrepare();
            return rolled_back ? connected : rolled_back;
        }

        state_->dirty_processing.clear();
        state_->retirement_queue.clear();
        state_->prepare_state = EPrepareState::PREPARED;
        return {};
    }

    lux::cxx::expected<void, EScriptSystemError>
    ScriptSystem::processLifecycle(EScriptLifecycleAdmission admission) noexcept
    {
        if (!state_ || state_->prepare_state == EPrepareState::SHUT_DOWN)
            return lux::cxx::unexpected(EScriptSystemError::SHUT_DOWN);
        State::ExecutionOwnerScope execution{*state_};
        if (!execution)
            return lux::cxx::unexpected(EScriptSystemError::ENDPOINT_BUSY);
        if (state_->endpoint_dispatch_depth != 0U || state_->instance_owner.protectedCount() != 0U ||
            state_->result_write_pins != 0U || state_->active_claimed_waiters != 0U)
            return lux::cxx::unexpected(EScriptSystemError::ENDPOINT_BUSY);
        if (state_->stopping)
            return admission == EScriptLifecycleAdmission::RETIRE_ONLY ? shutdown() :
                lux::cxx::expected<void, EScriptSystemError>{lux::cxx::unexpected(EScriptSystemError::ENDPOINT_BUSY)};
        if (state_->prepare_state == EPrepareState::ROLLBACK_PENDING)
            return lux::cxx::unexpected(EScriptSystemError::ENDPOINT_BUSY);
        if (state_->prepare_state != EPrepareState::PREPARED)
            return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);

        state_->lifecycle_retirements.clear();
        state_->lifecycle_candidates.clear();
        for (const auto mount_slot : state_->retirement_queue)
        {
            const auto mount = state_->instance_owner.view(mount_slot);
            if (mount.state == EScriptMountState::FAULTED)
            {
                state_->lifecycle_retirements.push_back(state_->beginRetirement(
                    mount_slot,
                    EScriptEndPlayReason::FAULTED,
                    EScriptMountState::FAULTED,
                    true
                ));
            }
        }
        state_->dirty_processing.clear();
        std::swap(state_->dirty_current, state_->dirty_processing);
        std::optional<EScriptSystemError> first_error;
        for (const auto mount_slot : state_->dirty_processing.values)
        {
            const auto mount = state_->instance_owner.view(mount_slot);
            if (mount.state == EScriptMountState::FAULTED ||
                (mount.state == EScriptMountState::RETIRING && mount.retirement_queued))
                continue;

            const bool attachment_matches = state_->ownsAttachment(mount_slot, mount.entity);
            if (mount.state == EScriptMountState::ACTIVE && attachment_matches)
                continue;

            if (mount.state != EScriptMountState::INACTIVE)
            {
                state_->lifecycle_retirements.push_back(state_->beginRetirement(
                    mount_slot,
                    mount.pending_end_reason,
                    EScriptMountState::INACTIVE,
                    attachment_matches
                ));
            }
            state_->lifecycle_candidates.push_back(mount_slot);
        }
        state_->retirement_queue.clear();
        state_->dirty_processing.clear();

        for (const auto& retirement : state_->lifecycle_retirements)
            state_->finishRetirement(retirement);

        if (admission == EScriptLifecycleAdmission::RETIRE_ONLY)
        {
            for (const auto mount_slot : state_->lifecycle_candidates)
                state_->queueDirty(mount_slot);
            return {};
        }

        std::sort(state_->lifecycle_candidates.begin(), state_->lifecycle_candidates.end(),
            [&](std::uint32_t left, std::uint32_t right) noexcept {
                const auto first = state_->instance_owner.view(left).admission_order;
                return first < state_->instance_owner.view(right).admission_order;
            });
        state_->lifecycle_initialized.clear();
        for (const auto mount_slot : state_->lifecycle_candidates)
        {
            const auto initialized = state_->initializeMount(mount_slot);
            if (initialized)
            {
                if (state_->instance_owner.view(mount_slot).state == EScriptMountState::INITIALIZED)
                    state_->lifecycle_initialized.push_back(mount_slot);
                continue;
            }

            const auto error = initialized.error();
            if (!first_error)
                first_error = error;

            state_->instance_owner.reject(mount_slot, error);
            state_->recordFailure(error, mount_slot);
        }

        for (const auto mount_slot : state_->lifecycle_initialized)
        {
            const auto begun = state_->beginPlayMount(mount_slot);
            if (!begun)
            {
                state_->releaseMount(mount_slot, EScriptMountState::FAULTED, true);
                if (!first_error)
                    first_error = begun.error();
            }
        }
        for (const auto mount_slot : state_->lifecycle_initialized)
        {
            const auto mount = state_->instance_owner.view(mount_slot);
            if (mount.state != EScriptMountState::INITIALIZED || !mount.gameplay_lifetime_started)
                continue;
            const auto published = state_->publishMount(mount_slot);
            if (!published)
            {
                const auto error = published.error();
                state_->releaseMount(
                    mount_slot,
                    EScriptMountState::FAULTED,
                    true,
                    EScriptEndPlayReason::FAULTED
                );
                state_->recordFailure(error, mount_slot);
                if (!first_error)
                    first_error = error;
            }
        }
        for (const auto mount_slot : state_->lifecycle_initialized)
        {
            if (state_->instance_owner.view(mount_slot).state == EScriptMountState::INITIALIZED &&
                state_->instance_owner.view(mount_slot).gameplay_lifetime_started)
            {
                state_->activateMount(mount_slot);
            }
        }

        return first_error ? lux::cxx::expected<void, EScriptSystemError>{lux::cxx::unexpected(*first_error)}
                           : lux::cxx::expected<void, EScriptSystemError>{};
    }

    lux::cxx::expected<void, EScriptSystemError> ScriptSystem::executeStablePoint() noexcept
    {
        if (!state_ || state_->prepare_state == EPrepareState::SHUT_DOWN)
            return lux::cxx::unexpected(EScriptSystemError::SHUT_DOWN);
        State::ExecutionOwnerScope execution{*state_};
        if (!execution || state_->endpoint_dispatch_depth != 0U || state_->instance_owner.protectedCount() != 0U)
            return lux::cxx::unexpected(EScriptSystemError::ENDPOINT_BUSY);
        const auto step = state_->clock->snapshot().step_index;
        // Step zero is the standalone, not-yet-executing test/preparation boundary.
        if (step != 0U && step == state_->last_stable_step)
            return {};
        state_->last_stable_step = step;
        if (!state_->external_admission_prepared)
            beginStableAdmission();
        state_->external_admission_prepared = false;
        const auto lifecycle = processLifecycle();
        std::optional<EScriptSystemError> first_error;
        if (!lifecycle)
            first_error = lifecycle.error();
        if (!state_->promoteNextStepWaits() && !first_error)
            first_error = EScriptSystemError::INVOCATION_FAILURE;
        if (!state_->promoteSimulationDelays() && !first_error)
            first_error = EScriptSystemError::INVOCATION_FAILURE;
        if (!state_->drainExternalCompletions() && !first_error)
            first_error = EScriptSystemError::INVOCATION_FAILURE;

        const auto resumed = state_->drainResumes();
        if (!resumed && !first_error)
            first_error = resumed.error();

        return first_error ? lux::cxx::expected<void, EScriptSystemError>(lux::cxx::unexpected(*first_error))
                           : lux::cxx::expected<void, EScriptSystemError>{};
    }

    lux::cxx::expected<void, EScriptSystemError>
    ScriptSystem::mountResolvedBatch(std::span<const ScriptRuntimeMount> mounts) noexcept
    {
        if (!state_ || state_->stopping || state_->prepare_state == EPrepareState::SHUT_DOWN)
            return lux::cxx::unexpected(EScriptSystemError::SHUT_DOWN);
        State::ExecutionOwnerScope execution{*state_};
        const bool busy = !execution || state_->prepare_state != EPrepareState::PREPARED ||
            state_->endpoint_dispatch_depth != 0U || state_->instance_owner.protectedCount() != 0U;
        if (busy)
            return lux::cxx::unexpected(EScriptSystemError::ENDPOINT_BUSY);
        return state_->acceptMounts(mounts);
    }

    lux::cxx::expected<std::optional<ScriptMountStatus>, EScriptSystemError>
    ScriptSystem::queryMountStatus(ScriptMountId id) const noexcept
    {
        if (!state_)
            return lux::cxx::unexpected(EScriptSystemError::SHUT_DOWN);
        State::ExecutionOwnerScope execution{*state_};
        if (!execution || state_->instance_owner.protectedCount() != 0U || state_->endpoint_dispatch_depth != 0U)
            return lux::cxx::unexpected(EScriptSystemError::ENDPOINT_BUSY);
        return state_->instance_owner.query(id);
    }

    lux::cxx::expected<ScriptMountStatusCollection, EScriptSystemError>
    ScriptSystem::collectMountStatusChanges(std::span<ScriptMountStatus> output) noexcept
    {
        if (!state_)
            return lux::cxx::unexpected(EScriptSystemError::SHUT_DOWN);
        State::ExecutionOwnerScope execution{*state_};
        if (!execution || state_->instance_owner.protectedCount() != 0U || state_->endpoint_dispatch_depth != 0U)
            return lux::cxx::unexpected(EScriptSystemError::ENDPOINT_BUSY);
        return state_->instance_owner.collect(output);
    }

    void ScriptSystem::beginStableAdmission() noexcept
    {
        if (!state_ || state_->stopping)
            return;
        state_->external_admission_frontier =
            state_->ingress->completions.enqueue_position.load(std::memory_order_acquire);
        state_->external_admission_remaining = state_->ingress->completions.capacity;
        state_->external_admission_prepared = true;
    }

    lux::cxx::expected<void, EScriptSystemError> ScriptSystem::shutdown() noexcept
    {
        if (!state_ || state_->prepare_state == EPrepareState::SHUT_DOWN)
            return {};
        State::ExecutionOwnerScope execution{*state_};
        if (!execution)
            return lux::cxx::unexpected(EScriptSystemError::ENDPOINT_BUSY);

        state_->stopping = true;
        state_->ingress->completions.stop();
        if (state_->instance_owner.protectedCount() != 0U || state_->result_write_pins != 0U)
            return lux::cxx::unexpected(EScriptSystemError::ENDPOINT_BUSY);
        state_->next_step_waits.clear();
        state_->next_step_first = {};
        state_->next_step_last = {};
        {
            state_->simulation_delays.clear();
        }

        const auto disconnected = state_->disconnectEndpoints();
        if (!disconnected)
            return disconnected;

        state_->releaseSignals();
        state_->lifecycle_retirements.clear();
        for (std::size_t index{}; index < state_->instance_owner.capacity(); ++index)
        {
            const auto mount = state_->instance_owner.view(index);
            if (mount.state == EScriptMountState::INACTIVE && mount.reclaimed)
                continue;
            const auto reason = mount.state == EScriptMountState::FAULTED
                ? EScriptEndPlayReason::FAULTED
                : EScriptEndPlayReason::RUNTIME_STOPPED;
            state_->lifecycle_retirements.push_back(state_->beginRetirement(
                static_cast<std::uint32_t>(index),
                reason,
                EScriptMountState::INACTIVE,
                true
            ));
        }
        for (auto iterator = state_->lifecycle_retirements.rbegin();
             iterator != state_->lifecycle_retirements.rend();
             ++iterator)
        {
            state_->finishRetirement(*iterator);
        }

        state_->dirty_current.clear();
        state_->dirty_processing.clear();
        state_->retirement_queue.clear();
        state_->continuations.clear();
        state_->event_waiters.clear();
        state_->event_wait_routes.clear();
        state_->claimed_event_waiters.clear();
        state_->execution_instances.clear();
        state_->active_hooks.clear();
        state_->preparer.releaseCatalog();
        state_->awaitables.clear();
        state_->resumes.clear();
        state_->instance_owner.finishShutdown();
        state_->prepare_state = EPrepareState::SHUT_DOWN;
        return {};
    }

    std::size_t ScriptSystem::activeInstanceCount() const noexcept
    {
        return state_ ? state_->instance_owner.activeCount() : 0U;
    }

    std::size_t ScriptSystem::activeContinuationCount() const noexcept
    {
        return state_ ? state_->continuations.size() : 0U;
    }

    std::size_t ScriptSystem::activeAwaitableCount() const noexcept
    {
        if (!state_ || !state_->ingress)
            return 0U;
        return state_->awaitables.size() - state_->pending_awaitable_releases;
    }

    ScriptRuntimeStats ScriptSystem::stats() const noexcept
    {
        ScriptRuntimeStats result;
        if (!state_)
            return result;
        State::ExecutionOwnerScope execution{*state_};
        if (!execution)
            return result;
        state_->instance_owner.writeStats(result);
        result.binding_backing_bytes = state_->binding_owner.backingBytes();
        result.assembly_endpoint_count_visits = state_->binding_owner.assemblyEndpointCountVisits();
        result.sync_invocations = state_->sync_invocations;
        result.step_invocations = state_->step_invocations;
        result.backend_resume_calls = state_->backend_resume_calls;
        result.suspensions_admitted = state_->suspensions_admitted;
        result.event_occurrences = state_->event_occurrences;
        result.active_continuations = state_->continuations.size();
        result.active_event_waiters = state_->event_waiters.size();
        result.event_waiter_high_water = state_->event_waiter_high_water;
        result.event_waiter_dispatch_visits = state_->event_waiter_dispatch_visits;
        result.instance_cleanup_event_waiter_visits = state_->instance_cleanup_event_waiter_visits;
        result.instance_cleanup_awaitable_visits = state_->instance_cleanup_awaitable_visits;
        result.instance_cleanup_continuation_visits = state_->instance_cleanup_continuation_visits;
        result.active_awaitables = state_->awaitables.size() - state_->pending_awaitable_releases;
        result.event_route_claim_lookups = state_->event_route_claim_lookups;
        result.event_payload_copy_bytes = state_->event_payload_copy_bytes;
        result.completion_capability_constructions = state_->completion_capability_constructions;
        result.result_write_pins = state_->result_write_pins;
        result.deferred_awaitable_releases = state_->pending_awaitable_releases;
        result.awaitable_record_bytes = sizeof(State::AwaitableRecord);
        result.event_waiter_record_bytes = sizeof(State::EventWaiterRecord);
        result.awaitable_reserved_slots = state_->awaitables.capacity();
        result.awaitable_storage_bytes = state_->awaitables.storageBytes();
        result.external_ticket_storage_bytes = state_->ingress->completions.ticket_capacity *
            sizeof(detail::ExternalCompletionRing::Ticket);
        result.resume_queue_depth = state_->resumes.count;
        result.resume_queue_high_water = state_->resumes.high_water;
        result.external_completion_queue_depth = state_->ingress->completions.count.load(std::memory_order_relaxed);
        result.external_completion_queue_high_water =
            state_->ingress->completions.high_water.load(std::memory_order_relaxed);
        result.external_completion_capacity_failures =
            state_->ingress->completions.capacity_failures.load(std::memory_order_relaxed);
        result.next_step_waits = state_->next_step_waits.size();
        {
            result.simulation_delay_waits = state_->simulation_delays.size();
        }
        return result;
    }

    std::span<const ScriptSystemFailure> ScriptSystem::failures() const noexcept
    {
        return state_ ? std::span<const ScriptSystemFailure>(state_->failures) : std::span<const ScriptSystemFailure>{};
    }
}
