#include <lux/engine/simulation/scripting/ScriptSignatureCompatibility.hpp>
#include <lux/engine/simulation/ScriptSystem.hpp>
#include <lux/cxx/container/StableSlotMap.hpp>
#include <lux/engine/simulation/script/ExternalCompletionRing.hpp>
#include <lux/engine/simulation/detail/DenseEntityHandlerStorage.hpp>
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
        constexpr std::size_t kBackendKindCount{7U};
        constexpr std::uint32_t kInvalidMethodSlot = std::numeric_limits<std::uint32_t>::max();

        enum class EBindingKind : std::uint8_t
        {
            HOOK,
            EVENT,
        };

        enum class EPrepareState : std::uint8_t
        {
            CREATED,
            PREPARING,
            ROLLBACK_PENDING,
            PREPARED,
            SHUT_DOWN,
        };

        struct EndpointKey final
        {
            std::uint64_t system{};
            std::uint64_t endpoint{};

            friend bool operator==(EndpointKey, EndpointKey) noexcept = default;
        };

        struct EndpointKeyHash final
        {
            [[nodiscard]] std::size_t operator()(EndpointKey key) const noexcept
            {
                const auto system_hash = std::hash<std::uint64_t>{}(key.system);
                const auto endpoint_hash = std::hash<std::uint64_t>{}(key.endpoint);
                return system_hash ^ (endpoint_hash + 0x9e3779b9U + (system_hash << 6U) + (system_hash >> 2U));
            }
        };

        [[nodiscard]] constexpr std::size_t backendIndex(lux::rdesc::Script::Kind kind) noexcept
        {
            return static_cast<std::size_t>(kind);
        }

        [[nodiscard]] EScriptSystemError backendError(EScriptBackendResult result) noexcept
        {
            if (result == EScriptBackendResult::CAPACITY_EXCEEDED)
                return EScriptSystemError::CAPACITY_EXCEEDED;
            if (result == EScriptBackendResult::ALLOCATION_FAILURE)
                return EScriptSystemError::ALLOCATION_FAILURE;
            return EScriptSystemError::BACKEND_FAILURE;
        }
    }

    struct ScriptSystem::State final
    {
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

        struct HandlerTag;
        struct Handler;
        using HandlerStorage = lux::cxx::SlotMap<Handler, HandlerTag>;
        using HandlerKey = typename HandlerStorage::key_type;

        struct Handler final
        {
            std::uint32_t mount_slot{};
            std::uint32_t method_slot{};
        };

        using EventHandlerStorage = lux::simulation::detail::DenseEntityHandlerStorage<Handler>;

        struct PreparedMethod final
        {
            lux::script::ScriptSymbolId symbol{};
            ScriptBackendPreparedMethod backend;
            ScriptContinuationId active_hook;
            bool used_by_binding{};
        };

        struct RuntimeBinding final
        {
            EBindingKind kind{EBindingKind::HOOK};
            std::uint32_t bucket_slot{};
            std::uint32_t method_slot{};
            EndpointConnectionToken registration;
        };

        struct RuntimeMount final
        {
            ScriptMountId id;
            lux::asset::AssetId asset;
            bool entity_scope{};
            std::optional<ScriptInstanceScope> pending_scope;
            ScriptMountStatus status;
            bool unconsumed_result{};
            std::uint64_t admission_order{};
            std::size_t binding_first{};
            std::size_t binding_count{};
            std::size_t method_first{};
            std::size_t method_count{};
            std::uint32_t begin_play_method{kInvalidMethodSlot};
            std::uint32_t end_play_method{kInvalidMethodSlot};
            ScriptInstanceScope scope;
            ScriptBehavior behavior;
            ScriptInstanceId instance;
            ScriptInstanceId retiring_instance;
            ScriptContinuationId retiring_continuations;
            std::vector<PreparedScriptApiCapability> capabilities;
            std::vector<PreparedScriptEventAdmission> event_sources;
            std::uint64_t event_layout_epoch{};
            ResolvedScriptArtifact artifact;
            const ScriptBackendDescriptor* backend{};
            ScriptBackendInstance backend_instance;
            ecs::Entity entity{ecs::NullEntity};
            EScriptMountState state{EScriptMountState::INACTIVE};
            bool active_counted{};
            bool retirement_queued{};
            bool gameplay_lifetime_started{};
            EScriptEndPlayReason pending_end_reason{EScriptEndPlayReason::OBJECT_UNMATERIALIZED};
        };

        struct RetirementRecord final
        {
            std::uint32_t mount_slot{};
            EScriptEndPlayReason reason{EScriptEndPlayReason::OBJECT_UNMATERIALIZED};
            EScriptMountState final_state{EScriptMountState::INACTIVE};
            bool invoke_end_play{};
        };

        struct InstanceTag;
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

        struct InstanceRecord final
        {
            ScriptInstanceId id;
            std::uint32_t mount_slot{};
            std::size_t active_continuations{};
            ScriptContinuationId first_continuation;
            ScriptAwaitableId first_awaitable;
            EventWaiterId first_event_waiter;
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

        using InstanceStorage = lux::cxx::SlotMap<InstanceRecord, InstanceTag>;
        using InstanceKey = typename InstanceStorage::key_type;
        using ContinuationStorage = lux::cxx::SlotMap<ContinuationRecord, ContinuationTag>;
        using ContinuationKey = typename ContinuationStorage::key_type;
        using AwaitableStorage = lux::cxx::StableSlotMap<AwaitableRecord, AwaitableTag>;
        using AwaitableKey = typename AwaitableStorage::key_type;
        using EventWaiterStorage = lux::cxx::SlotMap<EventWaiterRecord, EventWaiterTag>;
        using EventWaiterKey = typename EventWaiterStorage::key_type;
        using EventRouteIndex = entt::dense_map<EventRouteKey, EventRouteHead, EventRouteKeyHash>;

        [[nodiscard]] static constexpr ScriptInstanceId instanceId(InstanceKey key) noexcept
        {
            return {key.index + 1U, key.gen};
        }

        [[nodiscard]] static constexpr InstanceKey instanceKey(ScriptInstanceId id) noexcept
        {
            return id.valid() ? InstanceKey{id.slot - 1U, id.generation} : InstanceKey::invalid();
        }

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

        struct HookBucket final
        {
            State* owner{};
            const ScriptHookEndpointDescriptor* endpoint{};
            EndpointConnectionToken token;
            HandlerStorage handlers;
            std::size_t handler_capacity{};
        };

        struct EventBucket final
        {
            State* owner{};
            const ScriptEventEndpointDescriptor* endpoint{};
            std::uint32_t slot{};
            EndpointConnectionToken token;
            EventHandlerStorage handlers;
            std::size_t handler_capacity{};
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
        ScriptRuntimeCapacityPlan capacity;
        ecs::Registry* registry{};
        const SimulationClock* clock{};
        ScriptRuntimeLimits limits;
        ScriptArtifactResolver artifacts;
        ScriptHostApi host;
        ScriptRealDelayEndpoint real_delay;
        std::array<ScriptBackendDescriptor, kBackendKindCount> backends;
        std::vector<ScriptHookEndpointDescriptor> hook_endpoints;
        std::vector<ScriptEventEndpointDescriptor> event_endpoints;
        std::unordered_map<EndpointKey, std::uint32_t, EndpointKeyHash> hook_endpoint_index;
        std::unordered_map<EndpointKey, std::uint32_t, EndpointKeyHash> event_endpoint_index;
        std::vector<PreparedScriptApiCapability> published_capabilities;
        std::vector<RuntimeMount> mounts;
        std::size_t configured_mounts{};
        std::size_t pending_mounts{};
        std::uint64_t admission_sequence{};
        std::vector<ScriptBindingDescription> binding_descriptions;
        std::vector<std::pair<std::uint64_t, std::uint32_t>> mount_index;
        std::vector<std::uint64_t> batch_ids;
        std::vector<std::uint32_t> batch_slots;
        std::vector<std::uint8_t> reserved_mounts;
        std::vector<ecs::Entity> batch_entities;
        entt::dense_map<ecs::Entity, std::uint32_t> entity_associations;
        std::vector<std::size_t> hook_reservations;
        std::vector<std::size_t> hook_configured_counts;
        std::vector<std::size_t> event_reservations;
        std::vector<std::size_t> event_configured_counts;
        SparseMountQueue status_changes;
        std::vector<RuntimeBinding> bindings;
        std::vector<PreparedMethod> methods;
        std::vector<HookBucket> hooks;
        std::vector<EventBucket> events;
        std::vector<ScriptSystemFailure> failures;
        InstanceStorage instances;
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
        std::size_t user_invocation_depth{};
        ScriptEventAdmissionScope event_admission_scope;
        std::uint64_t next_event_layout_epoch{};
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
            explicit UserInvocationScope(State& state) noexcept : owner(state) { ++owner.user_invocation_depth; }
            ~UserInvocationScope() noexcept { --owner.user_invocation_depth; }
            State& owner;
        };
        entt::connection constructed;
        entt::connection updated;
        entt::connection destroyed;
        std::size_t active_mount_count{};
        bool suppress_attachment_signal{};
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

        [[nodiscard]] static constexpr EndpointConnectionToken hookToken(HandlerKey key) noexcept
        {
            return {key.index, key.gen};
        }

        [[nodiscard]] static constexpr HandlerKey hookKey(EndpointConnectionToken token) noexcept
        {
            return {token.slot, token.generation};
        }

        [[nodiscard]] const ScriptBackendDescriptor* backend(lux::rdesc::Script::Kind kind) const noexcept
        {
            const auto index = backendIndex(kind);
            if (index >= backends.size() || backends[index].kind != kind)
                return nullptr;
            return std::addressof(backends[index]);
        }

        [[nodiscard]] const PreparedScriptApiCapability* capability(
            const lux::script::ScriptApiContractId& contract) const noexcept
        {
            const auto found =
                std::find_if(published_capabilities.begin(),
                             published_capabilities.end(),
                             [&contract](const auto& value) noexcept { return value.contract == contract; });
            return found != published_capabilities.end() ? std::addressof(*found) : nullptr;
        }

        [[nodiscard]] bool validInstance(ScriptInstanceId instance) const noexcept
        {
            return instances.find(instanceKey(instance)) != nullptr;
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

        [[nodiscard]] lux::cxx::expected<ScriptInstanceId, EScriptSystemError> createInstanceRecord(
            std::uint32_t mount_slot) noexcept
        {
            if (instances.size() >= limits.instance_capacity)
                return lux::cxx::unexpected(EScriptSystemError::CAPACITY_EXCEEDED);
            auto inserted = instances.tryEmplace(InstanceRecord{{}, mount_slot, 0U});
            if (!inserted)
                return lux::cxx::unexpected(EScriptSystemError::ALLOCATION_FAILURE);
            const auto id = instanceId(*inserted);
            instances[*inserted].id = id;
            return id;
        }

        [[nodiscard]] lux::cxx::expected<ScriptAwaitableId, EScriptAwaitableCreateError> createAwaitableRecord(
            ScriptInstanceId instance,
            std::optional<PreparedResumeType> result_type,
            bool external_completion = true
        ) noexcept
        {
            auto* owner = instances.find(instanceKey(instance));
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
                                                                         this,
                                                                         &State::completeAbilityOwnerErased,
                                                                         &State::failAbilityOwnerErased}};
        }

        [[nodiscard]] static lux::cxx::expected<ScriptAwaitableRegistration, EScriptAwaitableCreateError>
        createAwaitableErased(void* context,
                              ScriptInstanceId instance,
                              std::optional<PreparedResumeType> result_type) noexcept
        {
            return static_cast<State*>(context)->createAwaitable(instance, std::move(result_type));
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
            auto& owner = *static_cast<State*>(context);
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
            auto& owner = *static_cast<State*>(context);
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
            auto* owner = instances.find(instanceKey(waiter.instance));
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
            auto* owner = instances.find(instanceKey(instance));
            if (owner == nullptr || owner->mount_slot >= mounts.size())
                return lux::cxx::unexpected(EScriptEventWaitError::INVALID_INSTANCE);
            auto& mount = mounts[owner->mount_slot];
            if (mount.state != EScriptMountState::ACTIVE || mount.instance != instance)
                return lux::cxx::unexpected(EScriptEventWaitError::INVALID_INSTANCE);
            const bool invalid_source = admission.scope_ != event_admission_scope || admission.instance_ != instance ||
                admission.layout_epoch_ != mount.event_layout_epoch ||
                admission.local_slot_ >= mount.event_sources.size();
            if (invalid_source)
                return lux::cxx::unexpected(EScriptEventWaitError::UNDECLARED_SOURCE);
            const auto& prepared = mount.event_sources[admission.local_slot_];
            const auto endpoint_slot = prepared.endpoint_slot;
            auto& bucket = events[endpoint_slot];

            ecs::Entity target{ecs::NullEntity};
            if (bucket.endpoint->route == EEventRoute::ENTITY_TARGETED)
            {
                const auto* entity_scope = std::get_if<EntityScriptScope>(&mount.scope);
                if (entity_scope == nullptr || entity_scope->self == ecs::NullEntity ||
                    !registry->valid(entity_scope->self))
                {
                    return lux::cxx::unexpected(EScriptEventWaitError::SCOPE_MISMATCH);
                }
                target = entity_scope->self;
            }

            const auto reserved_waiters = event_waiters.size() - active_claimed_waiters + claimed_event_waiters.size();
            if (reserved_waiters >= limits.event_wait_capacity)
                return lux::cxx::unexpected(EScriptEventWaitError::WAITER_CAPACITY_EXCEEDED);
            if (event_wait_sequence == std::numeric_limits<std::uint64_t>::max())
                return lux::cxx::unexpected(EScriptEventWaitError::SEQUENCE_EXHAUSTED);

            auto awaitable = createAwaitableRecord(instance, prepared.payload, false);
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
            return static_cast<State*>(context)->waitEvent(instance, admission);
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
            auto* owner = instances.find(instanceKey(record.instance));
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
            static_cast<State*>(context)->discardAwaitable(instance, awaitable);
        }

        [[nodiscard]] std::optional<ResumeRecord> popResume() noexcept
        {
            return resumes.pop();
        }

        void clearActiveHook(const ContinuationRecord& continuation) noexcept
        {
            if (!continuation.hook_single_flight || continuation.method_slot >= methods.size())
                return;
            auto& method = methods[continuation.method_slot];
            if (method.active_hook == continuation.id)
                method.active_hook = {};
        }

        void destroyContinuation(ScriptContinuationId id) noexcept
        {
            auto* stored = continuations.find(continuationKey(id));
            if (stored == nullptr)
                return;
            const auto continuation = *stored;
            if (auto* instance = instances.find(instanceKey(continuation.instance)); instance != nullptr)
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

        void invalidateAdmission(RuntimeMount& mount) noexcept
        {
            const auto instance = mount.instance;
            if (!instance.valid())
                return;
            const auto* record = instances.find(instanceKey(instance));
            if (record == nullptr)
                std::terminate();
            const auto first_awaitable = record->first_awaitable;
            const auto first_continuation = record->first_continuation;
            const auto first_event_waiter = record->first_event_waiter;
            static_cast<void>(instances.erase(instanceKey(instance)));
            mount.retiring_instance = instance;
            mount.retiring_continuations = first_continuation;
            mount.instance = {};
            cancelEventWaiters(instance, first_event_waiter);
            cancelAwaitables(instance, first_awaitable);
        }

        void invalidateInstance(RuntimeMount& mount) noexcept
        {
            invalidateAdmission(mount);
            if (mount.retiring_instance.valid())
            {
                destroyContinuations(mount.retiring_instance, mount.retiring_continuations);
                mount.retiring_instance = {};
                mount.retiring_continuations = {};
            }
        }

        void faultInvocation(RuntimeMount& mount,
                             lux::script::ScriptSymbolId symbol,
                             EScriptSystemError error,
                             std::int32_t status = 0) noexcept
        {
            mount.state = EScriptMountState::FAULTED;
            markStatus(mount);
            mount.pending_end_reason = EScriptEndPlayReason::FAULTED;
            deactivate(mount);
            invalidateAdmission(mount);
            if (endpoint_dispatch_depth == 0U)
                removeMountBindings(mount);
            queueRetirement(static_cast<std::uint32_t>(std::addressof(mount) - mounts.data()));
            recordFailure(error, mount, symbol, status);
        }

        [[nodiscard]] bool beginSuspension(RuntimeMount& mount,
                                           PreparedMethod& method,
                                           ScriptBackendContinuation backend_continuation,
                                           ScriptStepResult result,
                                           bool hook_single_flight) noexcept
        {
            if (!result.valid() || result.state != EScriptStepState::SUSPENDED || !backend_continuation)
            {
                if (backend_continuation)
                    backend_continuation.destroy(backend_continuation.state);
                discardAwaitable(mount.instance, result.waiting_on);
                faultInvocation(mount, method.symbol, EScriptSystemError::INVOCATION_FAILURE);
                return false;
            }
            auto* instance = instances.find(instanceKey(mount.instance));
            if (instance == nullptr)
            {
                backend_continuation.destroy(backend_continuation.state);
                discardAwaitable(mount.instance, result.waiting_on);
                faultInvocation(mount, method.symbol, EScriptSystemError::INVOCATION_FAILURE);
                return false;
            }
            if (instance->active_continuations >= limits.continuation_capacity_per_instance)
            {
                backend_continuation.destroy(backend_continuation.state);
                discardAwaitable(mount.instance, result.waiting_on);
                faultInvocation(
                    mount,
                    method.symbol,
                    EScriptSystemError::INSTANCE_CONTINUATION_CAPACITY_EXCEEDED
                );
                return false;
            }
            if (continuations.size() >= limits.continuation_capacity)
            {
                backend_continuation.destroy(backend_continuation.state);
                discardAwaitable(mount.instance, result.waiting_on);
                faultInvocation(mount, method.symbol, EScriptSystemError::CONTINUATION_CAPACITY_EXCEEDED);
                return false;
            }
            auto inserted = continuations.tryEmplace(
                ContinuationRecord{{},
                                   mount.instance,
                                   backend_continuation,
                                   result.waiting_on,
                                   static_cast<std::uint32_t>(std::addressof(method) - methods.data()),
                                   hook_single_flight});
            if (!inserted)
            {
                backend_continuation.destroy(backend_continuation.state);
                discardAwaitable(mount.instance, result.waiting_on);
                faultInvocation(mount, method.symbol, EScriptSystemError::ALLOCATION_FAILURE);
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
                faultInvocation(mount, method.symbol, attached.error());
                return false;
            }
            if (hook_single_flight)
                method.active_hook = id;
            ++suspensions_admitted;
            return true;
        }

        [[nodiscard]] std::optional<std::uint32_t> findHookEndpoint(HookScriptTarget target) const noexcept
        {
            const auto found = hook_endpoint_index.find({target.system.value, target.hook.value});
            return found == hook_endpoint_index.end() ? std::nullopt : std::optional<std::uint32_t>{found->second};
        }

        [[nodiscard]] std::optional<std::uint32_t> findEventEndpoint(EventScriptTarget target) const noexcept
        {
            const auto found = event_endpoint_index.find({target.system.value, target.event.value});
            return found == event_endpoint_index.end() ? std::nullopt : std::optional<std::uint32_t>{found->second};
        }

        [[nodiscard]] bool eventRequirementMatches(
            const lux::script::ScriptEventSourceDescription& requirement,
            const SimulationEventView& described,
            const ScriptEventEndpointDescriptor& endpoint
        ) const noexcept
        {
            if (!described)
                return false;
            const auto expected_route = described.route() == EEventRoute::SIMULATION_BROADCAST
                ? lux::script::EScriptEventRoute::SIMULATION_BROADCAST
                : lux::script::EScriptEventRoute::ENTITY_TARGETED;
            const auto& owned = endpoint.payload_projection.owned_layout;
            const auto delivery = described.dispatchHook();
            const bool is_delivery_mismatch = requirement.delivery_hook_id != delivery.id().value ||
                requirement.delivery_schema_hash != delivery.contractHash() ||
                requirement.delivery_schema_version != delivery.contractVersion();
            if (is_delivery_mismatch)
                return false;
            return endpoint.system.value == requirement.system_id &&
                endpoint.event.value == requirement.event_id && expected_route == requirement.route &&
                endpoint.route == described.route() && described.payloadType() == requirement.payload.type_id &&
                described.payloadSchemaName() == requirement.payload.canonical_name &&
                described.payloadSchemaHash() == requirement.payload_schema_hash &&
                described.payloadSchemaVersion() == requirement.payload_schema_version &&
                endpoint.payload_type.type_id == requirement.payload.type_id &&
                endpoint.payload_type.canonical_name == requirement.payload.canonical_name &&
                endpoint.payload_type.pass == lux::semantic::EValuePass::CONST_REF &&
                owned.type_id == requirement.payload.type_id &&
                owned.canonical_name == requirement.payload.canonical_name &&
                owned.abi_kind == requirement.payload.abi_kind && owned.size == requirement.payload.size &&
                owned.alignment == requirement.payload.alignment && endpoint.payload_projection.copy != nullptr;
        }

        [[nodiscard]] std::optional<std::uint32_t> findMount(ScriptMountId id) const noexcept
        {
            const auto found = std::lower_bound(mount_index.begin(), mount_index.end(), id.value,
                [](const auto& entry, std::uint64_t value) noexcept { return entry.first < value; });
            if (found == mount_index.end() || found->first != id.value)
                return std::nullopt;
            return found->second;
        }

        void markStatus(RuntimeMount& mount) noexcept
        {
            if (!mount.id.valid())
                return;
            ++mount.status.revision;
            mount.status.state = mount.state;
            mount.status.instance = mount.instance;
            if (mount.state != EScriptMountState::INACTIVE)
                mount.status.scope = mount.scope;
            const auto slot = static_cast<std::uint32_t>(std::addressof(mount) - mounts.data());
            if (!status_changes.insert(slot))
                std::terminate(); // Prepared one-entry-per-configuration backing is an internal invariant.
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptSystemError> buildLayout() noexcept
        {
            try
            {
                mounts.resize(capacity.mount_capacity);
                bindings.reserve(capacity.binding_capacity);
                binding_descriptions.reserve(capacity.binding_capacity);
                methods.reserve(capacity.method_capacity);
                mount_index.reserve(capacity.enabled_mount_capacity);
                batch_ids.reserve(capacity.enabled_mount_capacity);
                batch_slots.reserve(capacity.enabled_mount_capacity);
                reserved_mounts.resize(capacity.mount_capacity);
                batch_entities.reserve(capacity.enabled_mount_capacity);
                entity_associations.reserve(capacity.enabled_mount_capacity * 2U);
                hooks.resize(hook_endpoints.size());
                hook_reservations.resize(hooks.size());
                hook_configured_counts.resize(hooks.size());
                for (std::size_t index{}; index < hooks.size(); ++index)
                {
                    hooks[index].owner = this;
                    hooks[index].endpoint = std::addressof(hook_endpoints[index]);
                }
                events.resize(event_endpoints.size());
                event_reservations.resize(events.size());
                event_configured_counts.resize(events.size());
                for (std::size_t index{}; index < events.size(); ++index)
                {
                    events[index].owner = this;
                    events[index].endpoint = std::addressof(event_endpoints[index]);
                    events[index].slot = static_cast<std::uint32_t>(index);
                }
                for (const auto& planned : capacity.endpoint_capacities)
                {
                    if (const auto* target = std::get_if<HookScriptTarget>(&planned.target))
                    {
                        const auto endpoint = findHookEndpoint(*target);
                        if (!endpoint)
                            return lux::cxx::unexpected(EScriptSystemError::SCRIPT_ENDPOINT_NOT_FOUND);
                        if (hooks[*endpoint].handler_capacity != 0U || planned.handler_capacity == 0U)
                            return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);
                        hooks[*endpoint].handler_capacity = planned.handler_capacity;
                    }
                    else
                    {
                        const auto endpoint = findEventEndpoint(std::get<EventScriptTarget>(planned.target));
                        if (!endpoint)
                            return lux::cxx::unexpected(EScriptSystemError::SCRIPT_ENDPOINT_NOT_FOUND);
                        if (events[*endpoint].handler_capacity != 0U || planned.handler_capacity == 0U)
                            return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);
                        events[*endpoint].handler_capacity = planned.handler_capacity;
                    }
                }
                for (auto& bucket : hooks)
                    bucket.handlers.reserve(bucket.handler_capacity);
                for (auto& bucket : events)
                {
                    if (bucket.handlers.prepare(bucket.handler_capacity) == EEndpointMutationError::ALLOCATION_FAILURE)
                        return lux::cxx::unexpected(EScriptSystemError::ALLOCATION_FAILURE);
                }
                dirty_current.prepare(mounts.size());
                dirty_processing.prepare(mounts.size());
                status_changes.prepare(mounts.size());
                retirement_queue.reserve(mounts.size());
                lifecycle_candidates.reserve(mounts.size());
                lifecycle_initialized.reserve(mounts.size());
                lifecycle_retirements.reserve(mounts.size());
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
            if (inputs.size() > capacity.enabled_mount_capacity)
                return lux::cxx::unexpected(EScriptSystemError::CAPACITY_EXCEEDED);
            batch_ids.clear();
            batch_slots.clear();
            std::fill(reserved_mounts.begin(), reserved_mounts.end(), 0U);
            std::uint32_t next_slot{};
            batch_entities.clear();
            for (const auto& input : inputs)
            {
                batch_ids.push_back(input.id.value);
                if (const auto* entity = std::get_if<EntityScriptScope>(&input.scope))
                    batch_entities.push_back(entity->self);
            }
            std::sort(batch_ids.begin(), batch_ids.end());
            std::sort(batch_entities.begin(), batch_entities.end());
            const bool duplicate_id = std::adjacent_find(batch_ids.begin(), batch_ids.end()) != batch_ids.end();
            const bool duplicate_entity =
                std::adjacent_find(batch_entities.begin(), batch_entities.end()) != batch_entities.end();
            if (duplicate_id || duplicate_entity)
                return lux::cxx::unexpected(EScriptSystemError::SCOPE_MISMATCH);

            std::copy(hook_configured_counts.begin(), hook_configured_counts.end(), hook_reservations.begin());
            std::copy(event_configured_counts.begin(), event_configured_counts.end(), event_reservations.begin());
            auto mount_count = configured_mounts;
            auto binding_count = bindings.size();
            auto method_count = methods.size();
            for (const auto& input : inputs)
            {
                const bool invalid_identity = !input.id.valid() || input.asset.isNull();
                const bool invalid_scope = input.scope.valueless_by_exception();
                if (invalid_identity || invalid_scope)
                    return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);
                const auto existing = findMount(input.id);
                auto slot = input.configuration_index;
                if (existing)
                {
                    if (slot != kInvalidMethodSlot && slot != *existing)
                        return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);
                    slot = *existing;
                }
                else
                {
                    if (slot == kInvalidMethodSlot)
                    {
                        if (prepare_state != EPrepareState::CREATED)
                            return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);
                        while (next_slot < mounts.size() &&
                            (mounts[next_slot].id.valid() || reserved_mounts[next_slot] != 0U))
                            ++next_slot;
                        slot = next_slot;
                    }
                    if (slot >= mounts.size() || mounts[slot].id.valid() || reserved_mounts[slot] != 0U)
                        return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);
                    reserved_mounts[slot] = 1U;
                }
                batch_slots.push_back(slot);
                if (const auto* entity = std::get_if<EntityScriptScope>(&input.scope))
                {
                    if (entity->self == ecs::NullEntity || !registry->valid(entity->self))
                        return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);
                    if (registry->all_of<detail::ScriptAttachment>(entity->self))
                        return lux::cxx::unexpected(EScriptSystemError::SCOPE_MISMATCH);
                    if (entity_associations.contains(entity->self))
                        return lux::cxx::unexpected(EScriptSystemError::SCOPE_MISMATCH);
                }
                if (existing)
                {
                    const auto& mount = mounts[*existing];
                    const bool invalid_shape = mount.asset != input.asset ||
                        mount.entity_scope != std::holds_alternative<EntityScriptScope>(input.scope) ||
                        mount.binding_count != input.bindings.size();
                    if (invalid_shape || !std::equal(input.bindings.begin(), input.bindings.end(),
                            binding_descriptions.begin() + mount.binding_first))
                        return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);
                    if (mount.state == EScriptMountState::FAULTED ||
                        std::holds_alternative<SimulationScriptScope>(input.scope))
                        return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);
                    if (mount.pending_scope || mount.unconsumed_result ||
                        (mount.state != EScriptMountState::INACTIVE && mount.state != EScriptMountState::RETIRING))
                        return lux::cxx::unexpected(EScriptSystemError::ENDPOINT_BUSY);
                    continue;
                }
                if (++mount_count > capacity.enabled_mount_capacity ||
                    input.bindings.size() > capacity.binding_capacity - binding_count)
                    return lux::cxx::unexpected(EScriptSystemError::CAPACITY_EXCEEDED);
                binding_count += input.bindings.size();
                std::size_t unique_methods{2U};
                for (std::size_t index{}; index < input.bindings.size(); ++index)
                {
                    const auto& binding = input.bindings[index];
                    if (binding.symbol == lux::script::InvalidScriptSymbolId || binding.target.valueless_by_exception())
                        return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);
                    bool seen_symbol{};
                    for (std::size_t previous{}; previous < index; ++previous)
                    {
                        if (input.bindings[previous] == binding)
                            return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);
                        seen_symbol = seen_symbol || input.bindings[previous].symbol == binding.symbol;
                    }
                    unique_methods += !seen_symbol;
                    if (const auto* target = std::get_if<HookScriptTarget>(&binding.target))
                    {
                        const auto endpoint = findHookEndpoint(*target);
                        if (!endpoint)
                            return lux::cxx::unexpected(EScriptSystemError::SCRIPT_ENDPOINT_NOT_FOUND);
                        if (++hook_reservations[*endpoint] > hooks[*endpoint].handler_capacity)
                            return lux::cxx::unexpected(EScriptSystemError::CAPACITY_EXCEEDED);
                    }
                    else
                    {
                        const auto endpoint = findEventEndpoint(std::get<EventScriptTarget>(binding.target));
                        if (!endpoint)
                            return lux::cxx::unexpected(EScriptSystemError::SCRIPT_ENDPOINT_NOT_FOUND);
                        if (std::holds_alternative<SimulationScriptScope>(input.scope) &&
                            events[*endpoint].endpoint->route == EEventRoute::ENTITY_TARGETED)
                            return lux::cxx::unexpected(EScriptSystemError::SCOPE_MISMATCH);
                        if (++event_reservations[*endpoint] > events[*endpoint].handler_capacity)
                            return lux::cxx::unexpected(EScriptSystemError::CAPACITY_EXCEEDED);
                    }
                }
                if (unique_methods > capacity.method_capacity - method_count)
                    return lux::cxx::unexpected(EScriptSystemError::CAPACITY_EXCEEDED);
                method_count += unique_methods;
            }

            std::copy(hook_reservations.begin(), hook_reservations.end(), hook_configured_counts.begin());
            std::copy(event_reservations.begin(), event_reservations.end(), event_configured_counts.begin());
            // Everything below uses prepared backing and non-throwing value copies. No user callback or allocation.
            for (std::size_t index{}; index < inputs.size(); ++index)
            {
                const auto& input = inputs[index];
                const auto existing = findMount(input.id);
                const auto slot = batch_slots[index];
                if (!existing)
                    ++configured_mounts;
                auto& mount = mounts[slot];
                if (!existing)
                {
                    mount.id = input.id;
                    mount.asset = input.asset;
                    mount.entity_scope = std::holds_alternative<EntityScriptScope>(input.scope);
                    mount.binding_first = bindings.size();
                    mount.method_first = methods.size();
                    mount.status.id = input.id;
                    for (const auto& binding : input.bindings)
                    {
                        RuntimeBinding runtime;
                        auto method = std::find_if(methods.begin() + mount.method_first, methods.end(),
                            [&](const auto& prepared) noexcept { return prepared.symbol == binding.symbol; });
                        if (method == methods.end())
                        {
                            runtime.method_slot = static_cast<std::uint32_t>(methods.size());
                            methods.push_back({binding.symbol, {}, {}, true});
                        }
                        else
                            runtime.method_slot = static_cast<std::uint32_t>(method - methods.begin());
                        if (const auto* target = std::get_if<HookScriptTarget>(&binding.target))
                        {
                            runtime.kind = EBindingKind::HOOK;
                            runtime.bucket_slot = *findHookEndpoint(*target);
                        }
                        else
                        {
                            runtime.kind = EBindingKind::EVENT;
                            runtime.bucket_slot = *findEventEndpoint(std::get<EventScriptTarget>(binding.target));
                        }
                        bindings.push_back(runtime);
                        binding_descriptions.push_back(binding);
                    }
                    methods.push_back({});
                    methods.push_back({});
                    mount.binding_count = bindings.size() - mount.binding_first;
                    mount.method_count = methods.size() - mount.method_first;
                    const auto position = std::lower_bound(mount_index.begin(), mount_index.end(), input.id.value,
                        [](const auto& entry, std::uint64_t value) noexcept { return entry.first < value; });
                    mount_index.insert(position, {input.id.value, slot});
                }
                mount.admission_order = ++admission_sequence;
                mount.pending_scope = input.scope;
                ++pending_mounts;
                if (const auto* entity = std::get_if<EntityScriptScope>(&input.scope))
                    entity_associations.emplace(entity->self, slot);
                ++mount.status.submission;
                mount.status.submission_state = EScriptMountSubmissionState::ACCEPTED;
                mount.status.submitted_scope = input.scope;
                markStatus(mount);
                if (prepare_state == EPrepareState::PREPARED)
                    queueDirty(slot);
            }
            return {};
        }

        void recordFailure(EScriptSystemError error,
                           RuntimeMount& mount,
                           lux::script::ScriptSymbolId symbol = 0U,
                           std::int32_t status = 0) noexcept
        {
            mount.status.submission_error = error;
            if (failures.size() < limits.failure_capacity)
                failures.push_back({error, mount.id, symbol, status});
        }

        void deactivate(RuntimeMount& mount) noexcept
        {
            if (!mount.active_counted)
                return;
            mount.active_counted = false;
            --active_mount_count;
        }

        void queueRetirement(std::uint32_t mount_slot) noexcept
        {
            auto& mount = mounts[mount_slot];
            if (mount.retirement_queued)
                return;
            if (retirement_queue.size() >= retirement_queue.capacity())
                std::terminate();

            mount.retirement_queued = true;
            retirement_queue.push_back(mount_slot);
        }

        void invoke(Handler& handler, lux_script_call_frame& frame, bool hook_invocation) noexcept
        {
            auto& mount = mounts[handler.mount_slot];
            if (stopping || mount.state != EScriptMountState::ACTIVE)
                return;

            auto& method = methods[handler.method_slot];
            if (hook_invocation && method.active_hook.valid() &&
                continuations.find(continuationKey(method.active_hook)) != nullptr)
            {
                return;
            }
            if (method.backend.resumable)
            {
                ScriptBackendContinuation continuation;
                ScriptStepContext context{
                    mount.instance,
                    this,
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
                if (mount.state != EScriptMountState::ACTIVE || stopping)
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
                        faultInvocation(mount, method.symbol, EScriptSystemError::INVOCATION_FAILURE);
                    }
                    return;
                }
                if (result.state == EScriptStepState::SUSPENDED)
                {
                    static_cast<void>(beginSuspension(mount, method, continuation, result, hook_invocation));
                    return;
                }
                if (continuation)
                    continuation.destroy(continuation.state);
                faultInvocation(mount, method.symbol, EScriptSystemError::INVOCATION_FAILURE, result.error.status);
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
            faultInvocation(mount, method.symbol, EScriptSystemError::INVOCATION_FAILURE, status);
        }

        static void invokeHookLane(void* context, lux_script_call_frame& frame) noexcept
        {
            auto& bucket = *static_cast<HookBucket*>(context);
            ExecutionOwnerScope execution{*bucket.owner};
            if (!execution)
                return;
            ++bucket.owner->endpoint_dispatch_depth;
            for (auto& handler : bucket.handlers.values())
                bucket.owner->invoke(handler, frame, true);
            --bucket.owner->endpoint_dispatch_depth;
        }

        void claimEventWaiters(EventBucket& bucket, ecs::Entity target, std::uint64_t cutoff) noexcept
        {
            ++event_route_claim_lookups;
            auto route = event_wait_routes.find(EventRouteKey{bucket.slot, target});
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
            auto* owner = instances.find(instanceKey(instance));
            if (owner == nullptr || owner->mount_slot >= mounts.size())
                return;
            auto& mount = mounts[owner->mount_slot];
            if (mount.instance != instance || mount.state != EScriptMountState::ACTIVE)
                return;
            faultInvocation(mount, lux::script::InvalidScriptSymbolId, error);
        }

        void completeClaimedEventWaiter(EventWaiterId id, lux_script_call_frame& frame) noexcept
        {
            auto* waiter = event_waiters.find(eventWaiterKey(id));
            if (waiter == nullptr || waiter->state != EEventWaiterState::CLAIMED)
                return;

            const auto instance = waiter->instance;
            const auto awaitable = waiter->awaitable;
            const auto* owner = instances.find(instanceKey(instance));
            const bool is_live = owner != nullptr && owner->mount_slot < mounts.size() &&
                mounts[owner->mount_slot].instance == instance &&
                mounts[owner->mount_slot].state == EScriptMountState::ACTIVE;
            if (!is_live)
            {
                eraseEventWaiter(id);
                discardAwaitable(instance, awaitable);
                return;
            }

            auto& bucket = events[waiter->bucket_slot];
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
                copied = !is_invalid_frame && bucket.endpoint->payload_projection.copy(
                    bucket.endpoint->context, frame.args[0], record->value.bytes.span());
            }
            const auto* current_owner = instances.find(instanceKey(instance));
            const bool still_live = !stopping && !record->release_pending && current_owner != nullptr &&
                current_owner->mount_slot < mounts.size() &&
                mounts[current_owner->mount_slot].instance == instance &&
                mounts[current_owner->mount_slot].state == EScriptMountState::ACTIVE;
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

        static void dispatchEvent(void* context, ecs::Entity entity, lux_script_call_frame& frame) noexcept
        {
            auto& bucket = *static_cast<EventBucket*>(context);
            auto& owner = *bucket.owner;
            ExecutionOwnerScope execution{owner};
            if (!execution)
                return;
            const auto claimed_begin = owner.claimed_event_waiters.size();
            ++owner.event_occurrences;
            const auto cutoff = owner.event_wait_sequence;
            ++owner.endpoint_dispatch_depth;
            const auto target = bucket.endpoint->route == EEventRoute::SIMULATION_BROADCAST
                ? ecs::NullEntity
                : entity;
            owner.claimEventWaiters(bucket, target, cutoff);
            const auto claimed_end = owner.claimed_event_waiters.size();
            const auto invoke = [&bucket, &frame](Handler& handler) noexcept {
                bucket.owner->invoke(handler, frame, false);
            };
            if (bucket.endpoint->route == EEventRoute::SIMULATION_BROADCAST)
                bucket.handlers.forEachAll(invoke);
            else
                bucket.handlers.forEachTarget(entity, invoke);

            for (std::size_t index{claimed_begin}; index < claimed_end; ++index)
                owner.completeClaimedEventWaiter(owner.claimed_event_waiters[index], frame);
            owner.claimed_event_waiters.resize(claimed_begin);
            --owner.endpoint_dispatch_depth;
        }

        [[nodiscard]] lux::cxx::expected<EndpointConnectionToken, EScriptSystemError> addEventHandler(
            EventBucket& bucket,
            std::uint32_t mount_slot,
            std::uint32_t method_slot,
            ecs::Entity target) noexcept
        {
            const bool is_broadcast = bucket.endpoint->route == EEventRoute::SIMULATION_BROADCAST;
            if (!is_broadcast && target == ecs::NullEntity)
                return lux::cxx::unexpected(EScriptSystemError::SCOPE_MISMATCH);

            const auto inserted = bucket.handlers.connect(target, Handler{mount_slot, method_slot}, is_broadcast);
            if (inserted)
                return inserted.token;
            const auto error = inserted.error == EEndpointMutationError::CAPACITY_EXCEEDED
                                   ? EScriptSystemError::CAPACITY_EXCEEDED
                                   : EScriptSystemError::ALLOCATION_FAILURE;
            return lux::cxx::unexpected(error);
        }

        void removeEventHandler(EventBucket& bucket, EndpointConnectionToken token) noexcept
        {
            static_cast<void>(bucket.handlers.disconnect(token));
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptSystemError> bindMount(std::uint32_t mount_slot) noexcept
        {
            auto& mount = mounts[mount_slot];
            const auto binding_end = mount.binding_first + mount.binding_count;
            for (std::size_t binding_slot{mount.binding_first}; binding_slot < binding_end; ++binding_slot)
            {
                auto& binding = bindings[binding_slot];
                if (binding.kind == EBindingKind::HOOK)
                {
                    auto& bucket = hooks[binding.bucket_slot];
                    if (bucket.handlers.size() >= bucket.handler_capacity)
                        return lux::cxx::unexpected(EScriptSystemError::CAPACITY_EXCEEDED);

                    const auto inserted = bucket.handlers.tryEmplace(Handler{mount_slot, binding.method_slot});
                    if (!inserted)
                        return lux::cxx::unexpected(EScriptSystemError::ALLOCATION_FAILURE);
                    binding.registration = hookToken(*inserted);
                    continue;
                }

                auto& bucket = events[binding.bucket_slot];
                const auto inserted = addEventHandler(bucket, mount_slot, binding.method_slot, mount.entity);
                if (!inserted)
                    return lux::cxx::unexpected(inserted.error());
                binding.registration = *inserted;
            }
            return {};
        }

        void removeMountBindings(RuntimeMount& mount) noexcept
        {
            const auto binding_end = mount.binding_first + mount.binding_count;
            for (std::size_t binding_slot{mount.binding_first}; binding_slot < binding_end; ++binding_slot)
            {
                auto& binding = bindings[binding_slot];
                if (!binding.registration.valid())
                    continue;

                if (binding.kind == EBindingKind::HOOK)
                    hooks[binding.bucket_slot].handlers.erase(hookKey(binding.registration));
                else
                    removeEventHandler(events[binding.bucket_slot], binding.registration);
                binding.registration = {};
            }
        }

        [[nodiscard]] bool ownsAttachment(std::uint32_t mount_slot, ecs::Entity entity) const noexcept
        {
            return entity != ecs::NullEntity && registry->valid(entity) &&
                   registry->all_of<detail::ScriptAttachment>(entity) &&
                   registry->get<detail::ScriptAttachment>(entity).mount_slot == mount_slot;
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptSystemError> projectAttachment(std::uint32_t mount_slot,
                                                                                     ecs::Entity entity) noexcept
        {
            if (registry->all_of<detail::ScriptAttachment>(entity))
            {
                const auto& attachment = registry->get<detail::ScriptAttachment>(entity);
                return attachment.mount_slot == mount_slot
                           ? lux::cxx::expected<void, EScriptSystemError>{}
                           : lux::cxx::expected<void, EScriptSystemError>(
                                 lux::cxx::unexpected(EScriptSystemError::SCOPE_MISMATCH));
            }

            suppress_attachment_signal = true;
            try
            {
                registry->emplace<detail::ScriptAttachment>(entity, mount_slot);
            }
            catch (const std::bad_alloc&)
            {
                suppress_attachment_signal = false;
                return lux::cxx::unexpected(EScriptSystemError::ALLOCATION_FAILURE);
            }
            suppress_attachment_signal = false;
            return {};
        }

        [[nodiscard]] static bool validBeginPlaySignature(const lux::rdesc::ScriptFunction& function) noexcept
        {
            return function.args.empty() && function.returns.empty();
        }

        [[nodiscard]] static bool validEndPlaySignature(const lux::rdesc::ScriptFunction& function) noexcept
        {
            if (function.args.size() != 1U || !function.returns.empty())
                return false;
            const auto& argument = function.args.front();
            using Traits = lux::semantic::TypeTraits<EScriptEndPlayReason>;
            return argument.canonical_name == Traits::CanonicalName &&
                argument.type_id == lux::semantic::typeId(Traits::CanonicalName) &&
                argument.pass == lux::semantic::EValuePass::VALUE && argument.abi_kind == Traits::AbiKind &&
                argument.size == Traits::Size && argument.alignment == Traits::Alignment;
        }

        [[nodiscard]] lux::cxx::expected<std::uint32_t, EScriptSystemError> claimLifecycleMethod(
            RuntimeMount& mount,
            lux::script::ScriptSymbolId symbol
        ) noexcept
        {
            if (symbol == lux::script::InvalidScriptSymbolId)
                return kInvalidMethodSlot;
            const auto method_end = mount.method_first + mount.method_count;
            for (std::size_t slot{mount.method_first}; slot < method_end; ++slot)
            {
                if (methods[slot].symbol == symbol)
                    return static_cast<std::uint32_t>(slot);
            }
            for (std::size_t slot{mount.method_first}; slot < method_end; ++slot)
            {
                if (methods[slot].symbol == lux::script::InvalidScriptSymbolId)
                {
                    methods[slot].symbol = symbol;
                    return static_cast<std::uint32_t>(slot);
                }
            }
            return lux::cxx::unexpected(EScriptSystemError::CAPACITY_EXCEEDED);
        }

        [[nodiscard]] int invokeLifecycle(
            PreparedMethod& method,
            const EScriptEndPlayReason* reason = nullptr
        ) noexcept
        {
            lux_script_call_frame frame{};
            lux_script_value_slot argument{};
            if (reason != nullptr)
            {
                argument = lux::simulation::script::detail::argumentSlot(*reason);
                frame.args = std::addressof(argument);
                frame.arg_count = 1U;
            }
            frame.user_context = method.backend.synchronous.context;
            UserInvocationScope scope(*this);
            return method.backend.synchronous.invoke(&frame);
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptSystemError> beginPlayMount(std::uint32_t mount_slot) noexcept
        {
            auto& mount = mounts[mount_slot];
            if (mount.state != EScriptMountState::INITIALIZED)
                return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);
            if (mount.begin_play_method != kInvalidMethodSlot)
            {
                auto& method = methods[mount.begin_play_method];
                const int status = invokeLifecycle(method);
                if (status != 0)
                {
                    recordFailure(EScriptSystemError::INVOCATION_FAILURE, mount, method.symbol, status);
                    return lux::cxx::unexpected(EScriptSystemError::INVOCATION_FAILURE);
                }
            }
            mount.gameplay_lifetime_started = true;
            return {};
        }

        void endPlayMount(RuntimeMount& mount, EScriptEndPlayReason reason) noexcept
        {
            if (mount.end_play_method == kInvalidMethodSlot)
                return;
            auto& method = methods[mount.end_play_method];
            const int status = invokeLifecycle(method, std::addressof(reason));
            if (status != 0)
                recordFailure(EScriptSystemError::INVOCATION_FAILURE, mount, method.symbol, status);
        }

        void removeOwnedAttachment(std::uint32_t mount_slot, ecs::Entity entity) noexcept
        {
            if (!ownsAttachment(mount_slot, entity))
                return;
            suppress_attachment_signal = true;
            registry->remove<detail::ScriptAttachment>(entity);
            suppress_attachment_signal = false;
        }

        void resetMountRuntime(RuntimeMount& mount) noexcept
        {
            if (mount.entity != ecs::NullEntity)
                entity_associations.erase(mount.entity);
            const auto method_end = mount.method_first + mount.method_count;
            for (std::size_t slot{mount.method_first}; slot < method_end; ++slot)
            {
                if (!methods[slot].used_by_binding)
                    methods[slot] = {};
            }
            mount.scope = SimulationScriptScope{};
            mount.behavior = {};
            mount.instance = {};
            mount.begin_play_method = kInvalidMethodSlot;
            mount.end_play_method = kInvalidMethodSlot;
            mount.capabilities.clear();
            mount.event_sources.clear();
            mount.artifact = {};
            mount.backend = nullptr;
            mount.backend_instance = {};
            mount.entity = ecs::NullEntity;
            mount.retirement_queued = false;
            mount.gameplay_lifetime_started = false;
            mount.pending_end_reason = EScriptEndPlayReason::OBJECT_UNMATERIALIZED;
        }

        [[nodiscard]] RetirementRecord beginRetirement(
            std::uint32_t mount_slot,
            EScriptEndPlayReason reason,
            EScriptMountState final_state,
            bool remove_attachment
        ) noexcept
        {
            auto& mount = mounts[mount_slot];
            const bool invoke_end_play = mount.gameplay_lifetime_started;
            mount.gameplay_lifetime_started = false;
            mount.status.retired_instance = mount.instance.valid() ? mount.instance : mount.retiring_instance;
            mount.state = EScriptMountState::RETIRING;
            markStatus(mount);
            deactivate(mount);
            removeMountBindings(mount);
            if (remove_attachment)
                removeOwnedAttachment(mount_slot, mount.entity);
            invalidateInstance(mount);
            return {mount_slot, reason, final_state, invoke_end_play};
        }

        void finishRetirement(const RetirementRecord& retirement) noexcept
        {
            auto& mount = mounts[retirement.mount_slot];
            UserInvocationScope cleanup(*this);
            if (retirement.invoke_end_play)
                endPlayMount(mount, retirement.reason);
            if (mount.backend != nullptr && mount.backend_instance)
            {
                const auto method_end = mount.method_first + mount.method_count;
                for (std::size_t index{method_end}; index > mount.method_first; --index)
                {
                    auto& method = methods[index - 1U];
                    if (!method.backend)
                        continue;
                    mount.backend->releaseMethod(
                        mount.backend->context,
                        mount.backend_instance,
                        method.backend
                    );
                    method.backend = {};
                }
                mount.backend->destroyInstance(mount.backend->context, mount.backend_instance);
            }
            mount.capabilities.clear();
            mount.event_sources.clear();
            if (mount.artifact.lease != nullptr && mount.artifact.release != nullptr)
                mount.artifact.release(mount.artifact.lease);

            resetMountRuntime(mount);
            mount.state = retirement.final_state;
            mount.status.reclaimed = true;
            if (mount.state == EScriptMountState::FAULTED)
            {
                mount.status.submission_state = EScriptMountSubmissionState::REJECTED;
                mount.unconsumed_result = true;
            }
            markStatus(mount);
        }

        void releaseMount(
            std::uint32_t mount_slot,
            EScriptMountState final_state,
            bool remove_attachment,
            EScriptEndPlayReason reason = EScriptEndPlayReason::OBJECT_UNMATERIALIZED
        ) noexcept
        {
            finishRetirement(beginRetirement(mount_slot, reason, final_state, remove_attachment));
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptSystemError> initializeMount(
            std::uint32_t mount_slot) noexcept
        {
            auto& mount = mounts[mount_slot];
            if (!mount.id.valid() || !mount.pending_scope || mount.state == EScriptMountState::FAULTED)
                return {};
            if (mount.state == EScriptMountState::ACTIVE || mount.state == EScriptMountState::INITIALIZED)
                return {};

            mount.scope = *mount.pending_scope;
            mount.pending_scope.reset();
            --pending_mounts;
            if (const auto* entity = std::get_if<EntityScriptScope>(&mount.scope))
            {
                const bool invalid_entity = entity->self == ecs::NullEntity || !registry->valid(entity->self);
                const bool occupied = !invalid_entity && registry->all_of<detail::ScriptAttachment>(entity->self);
                if (invalid_entity || occupied)
                {
                    entity_associations.erase(entity->self);
                    mount.state = EScriptMountState::INACTIVE;
                    mount.status.submission_state = EScriptMountSubmissionState::REJECTED;
                    mount.status.submission_error = occupied ? EScriptSystemError::SCOPE_MISMATCH :
                        EScriptSystemError::INVALID_INPUT;
                    mount.unconsumed_result = true;
                    markStatus(mount);
                    return {};
                }
                mount.entity = entity->self;
            }
            else
                mount.entity = ecs::NullEntity;
            mount.state = EScriptMountState::CONSTRUCTING;
            mount.status.reclaimed = false;
            markStatus(mount);
            mount.behavior.attach(mount.scope, host);

            if (!artifacts.resolve(artifacts.context, mount.asset, mount.artifact))
            {
                resetMountRuntime(mount);
                mount.state = EScriptMountState::INACTIVE;
                return lux::cxx::unexpected(EScriptSystemError::ASSET_NOT_RESIDENT);
            }
            if (mount.artifact.artifact == nullptr)
            {
                releaseMount(mount_slot, EScriptMountState::INACTIVE, false);
                return lux::cxx::unexpected(EScriptSystemError::INVALID_ASSET);
            }

            mount.backend = backend(mount.artifact.artifact->description().kind());
            if (mount.backend == nullptr)
            {
                releaseMount(mount_slot, EScriptMountState::INACTIVE, false);
                return lux::cxx::unexpected(EScriptSystemError::BACKEND_NOT_AVAILABLE);
            }

            const auto lifecycle = mount.artifact.artifact->description().lifecycle;
            const auto begin_method = claimLifecycleMethod(mount, lifecycle.begin_play);
            const auto end_method = claimLifecycleMethod(mount, lifecycle.end_play);
            if (!begin_method || !end_method)
            {
                const auto error = !begin_method ? begin_method.error() : end_method.error();
                releaseMount(mount_slot, EScriptMountState::INACTIVE, false);
                return lux::cxx::unexpected(error);
            }
            mount.begin_play_method = *begin_method;
            mount.end_play_method = *end_method;
            if (mount.begin_play_method != kInvalidMethodSlot)
            {
                const auto* function = mount.artifact.artifact->findExport(lifecycle.begin_play);
                if (function == nullptr || !validBeginPlaySignature(*function))
                {
                    releaseMount(mount_slot, EScriptMountState::INACTIVE, false);
                    return lux::cxx::unexpected(EScriptSystemError::SIGNATURE_MISMATCH);
                }
            }
            if (mount.end_play_method != kInvalidMethodSlot)
            {
                const auto* function = mount.artifact.artifact->findExport(lifecycle.end_play);
                if (function == nullptr || !validEndPlaySignature(*function))
                {
                    releaseMount(mount_slot, EScriptMountState::INACTIVE, false);
                    return lux::cxx::unexpected(EScriptSystemError::SIGNATURE_MISMATCH);
                }
            }

            try
            {
                const auto& requirements = mount.artifact.artifact->description().api_requirements;
                mount.capabilities.clear();
                mount.capabilities.reserve(requirements.size());
                for (const auto& requirement : requirements)
                {
                    const auto* resolved = capability(requirement.contract);
                    if (resolved == nullptr)
                    {
                        releaseMount(mount_slot, EScriptMountState::INACTIVE, false);
                        return lux::cxx::unexpected(EScriptSystemError::SCRIPT_CAPABILITY_NOT_FOUND);
                    }
                    if (resolved->schema_hash != requirement.expected_schema_hash)
                    {
                        releaseMount(mount_slot, EScriptMountState::INACTIVE, false);
                        return lux::cxx::unexpected(EScriptSystemError::SCRIPT_CAPABILITY_SCHEMA_MISMATCH);
                    }
                    for (const auto& method : resolved->methods)
                    {
                        if (method.kind != lux::script::EScriptApiMethodKind::ASYNC_OPERATION)
                            continue;
                        for (const auto& result : method.results)
                        {
                            const bool is_unsupported_result = result.size > limits.max_resume_payload_bytes ||
                                !supportsExternalResumeLayout(result.size, result.alignment);
                            if (is_unsupported_result)
                            {
                                releaseMount(mount_slot, EScriptMountState::INACTIVE, false);
                                return lux::cxx::unexpected(EScriptSystemError::SIGNATURE_MISMATCH);
                            }
                        }
                    }
                    mount.capabilities.push_back(*resolved);
                }

                const auto& event_requirements = mount.artifact.artifact->description().event_requirements;
                mount.event_sources.clear();
                mount.event_sources.reserve(event_requirements.size());
                if (next_event_layout_epoch == (std::numeric_limits<std::uint64_t>::max)())
                {
                    releaseMount(mount_slot, EScriptMountState::INACTIVE, false);
                    return lux::cxx::unexpected(EScriptSystemError::CAPACITY_EXCEEDED);
                }
                mount.event_layout_epoch = ++next_event_layout_epoch;
                for (const auto& requirement : event_requirements)
                {
                    if (requirement.payload.size > limits.max_resume_payload_bytes)
                    {
                        releaseMount(mount_slot, EScriptMountState::INACTIVE, false);
                        return lux::cxx::unexpected(EScriptSystemError::CAPACITY_EXCEEDED);
                    }
                    const auto endpoint_slot = findEventEndpoint({
                        lux::system::SystemInstanceId{requirement.system_id},
                        EventPointId{requirement.event_id}
                    });
                    if (!endpoint_slot)
                    {
                        releaseMount(mount_slot, EScriptMountState::INACTIVE, false);
                        return lux::cxx::unexpected(EScriptSystemError::SCRIPT_EVENT_NOT_FOUND);
                    }
                    const auto described = simulation->findEvent(
                        lux::system::SystemInstanceId{requirement.system_id},
                        EventPointId{requirement.event_id}
                    );
                    const auto& endpoint = event_endpoints[*endpoint_slot];
                    if (!eventRequirementMatches(requirement, described, endpoint))
                    {
                        releaseMount(mount_slot, EScriptMountState::INACTIVE, false);
                        return lux::cxx::unexpected(EScriptSystemError::SCRIPT_EVENT_SCHEMA_MISMATCH);
                    }
                    mount.event_sources.push_back({&requirement, {}, *endpoint_slot,
                        {requirement.payload.type_id, requirement.payload.abi_kind,
                            requirement.payload.size, requirement.payload.alignment}});
                }
            }
            catch (const std::bad_alloc&)
            {
                releaseMount(mount_slot, EScriptMountState::INACTIVE, false);
                return lux::cxx::unexpected(EScriptSystemError::ALLOCATION_FAILURE);
            }

            auto instance = createInstanceRecord(mount_slot);
            if (!instance)
            {
                const auto error = instance.error();
                releaseMount(mount_slot, EScriptMountState::INACTIVE, false);
                return lux::cxx::unexpected(error);
            }
            mount.instance = *instance;
            for (std::uint32_t local{}; local < mount.event_sources.size(); ++local)
            {
                auto& admission = mount.event_sources[local].admission;
                admission.scope_ = event_admission_scope;
                admission.instance_ = mount.instance;
                admission.layout_epoch_ = mount.event_layout_epoch;
                admission.local_slot_ = local;
            }

            const ScriptInstanceCreateContext create_context{mount.asset,
                                                             mount.scope,
                                                             std::addressof(mount.behavior),
                                                             mount.instance,
                                                             mount.capabilities,
                                                             mount.event_sources};
            const auto created = [&]() noexcept {
                UserInvocationScope scope(*this);
                return mount.backend->createInstance(mount.backend->context, create_context,
                    *mount.artifact.artifact, mount.backend_instance);
            }();
            if (created != EScriptBackendResult::SUCCESS)
            {
                const auto error = backendError(created);
                releaseMount(mount_slot, EScriptMountState::INACTIVE, false);
                return lux::cxx::unexpected(error);
            }

            const auto method_end = mount.method_first + mount.method_count;
            for (std::size_t method_slot{mount.method_first}; method_slot < method_end; ++method_slot)
            {
                auto& method = methods[method_slot];
                if (method.symbol == lux::script::InvalidScriptSymbolId)
                    continue;
                const auto* function = mount.artifact.artifact->findExport(method.symbol);
                if (function == nullptr)
                {
                    releaseMount(mount_slot, EScriptMountState::INACTIVE, false);
                    return lux::cxx::unexpected(EScriptSystemError::SYMBOL_NOT_FOUND);
                }

                const auto prepared_method = mount.backend->prepareMethod(
                    mount.backend->context,
                    mount.backend_instance,
                    *function,
                    method.backend
                );
                if (prepared_method != EScriptBackendResult::SUCCESS || !method.backend)
                {
                    const auto error = backendError(prepared_method);
                    releaseMount(mount_slot, EScriptMountState::INACTIVE, false);
                    return lux::cxx::unexpected(error);
                }
                const bool is_lifecycle = method_slot == mount.begin_play_method ||
                    method_slot == mount.end_play_method;
                const bool invalid_lifecycle_entry = is_lifecycle &&
                    (!method.backend.synchronous || static_cast<bool>(method.backend.resumable));
                if (invalid_lifecycle_entry)
                {
                    releaseMount(mount_slot, EScriptMountState::INACTIVE, false);
                    return lux::cxx::unexpected(EScriptSystemError::SIGNATURE_MISMATCH);
                }
            }

            const auto binding_end = mount.binding_first + mount.binding_count;
            for (std::size_t binding_slot{mount.binding_first}; binding_slot < binding_end; ++binding_slot)
            {
                const auto& binding = bindings[binding_slot];
                const auto* function = mount.artifact.artifact->findExport(methods[binding.method_slot].symbol);
                const bool is_hook = binding.kind == EBindingKind::HOOK;
                const bool is_signature_valid =
                    is_hook ? sameScriptHookSignature(*function, hooks[binding.bucket_slot].endpoint->signature)
                            : sameScriptEventSignature(*function, events[binding.bucket_slot].endpoint->payload_type);
                if (!is_signature_valid)
                {
                    releaseMount(mount_slot, EScriptMountState::INACTIVE, false);
                    return lux::cxx::unexpected(EScriptSystemError::SIGNATURE_MISMATCH);
                }

                if (!is_hook)
                {
                    const auto& endpoint = *events[binding.bucket_slot].endpoint;
                    const bool is_targeted = endpoint.route == EEventRoute::ENTITY_TARGETED;
                    const bool has_entity_scope = std::holds_alternative<EntityScriptScope>(mount.scope);
                    if (is_targeted && !has_entity_scope)
                    {
                        releaseMount(mount_slot, EScriptMountState::INACTIVE, false);
                        return lux::cxx::unexpected(EScriptSystemError::SCOPE_MISMATCH);
                    }
                }
            }

            mount.state = EScriptMountState::INITIALIZED;
            markStatus(mount);
            return {};
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptSystemError> publishMount(std::uint32_t mount_slot) noexcept
        {
            auto& mount = mounts[mount_slot];
            if (mount.state != EScriptMountState::INITIALIZED || !mount.gameplay_lifetime_started)
                return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);
            const auto bound = bindMount(mount_slot);
            if (!bound)
                return lux::cxx::unexpected(bound.error());
            if (mount.entity != ecs::NullEntity)
            {
                const auto projected = projectAttachment(mount_slot, mount.entity);
                if (!projected)
                {
                    removeMountBindings(mount);
                    return lux::cxx::unexpected(projected.error());
                }
            }
            return {};
        }

        void activateMount(std::uint32_t mount_slot) noexcept
        {
            auto& mount = mounts[mount_slot];
            mount.state = EScriptMountState::ACTIVE;
            mount.status.submission_state = EScriptMountSubmissionState::ACTIVATED;
            mount.unconsumed_result = true;
            markStatus(mount);
            mount.active_counted = true;
            ++active_mount_count;
        }

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
            if (suppress_attachment_signal || !source.all_of<detail::ScriptAttachment>(entity))
                return;

            const auto mount_slot = source.get<detail::ScriptAttachment>(entity).mount_slot;
            if (mount_slot >= mounts.size())
                return;
            auto& mount = mounts[mount_slot];
            if (mount.entity != entity)
                return;

            if (destroying && mount.state == EScriptMountState::ACTIVE)
            {
                mount.status.retired_instance = mount.instance.valid() ? mount.instance : mount.retiring_instance;
            mount.state = EScriptMountState::RETIRING;
            markStatus(mount);
                mount.pending_end_reason = EScriptEndPlayReason::ENTITY_DESTROYED;
                deactivate(mount);
                invalidateAdmission(mount);
                if (endpoint_dispatch_depth == 0U)
                    removeMountBindings(mount);
            }
            queueDirty(mount_slot);
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
            for (auto& bucket : hooks)
            {
                if (bucket.handler_capacity == 0U)
                    continue;
                const auto connected =
                    bucket.endpoint->connect(bucket.endpoint->context, std::addressof(bucket), &State::invokeHookLane);
                if (!connected)
                    return lux::cxx::unexpected(EScriptSystemError::ENDPOINT_CONNECTION_FAILURE);
                bucket.token = connected.token;
            }
            for (auto& bucket : events)
            {
                const auto& projection = bucket.endpoint->payload_projection;
                const bool supports_wait = projection.copy != nullptr &&
                    projection.owned_layout.size <= limits.max_resume_payload_bytes;
                if (bucket.handler_capacity == 0U && !supports_wait)
                    continue;
                const auto connected =
                    bucket.endpoint->connect(bucket.endpoint->context, std::addressof(bucket), &State::dispatchEvent);
                if (!connected)
                    return lux::cxx::unexpected(EScriptSystemError::ENDPOINT_CONNECTION_FAILURE);
                bucket.token = connected.token;
            }
            return {};
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptSystemError> disconnectEndpoints() noexcept
        {
            bool busy{};
            for (auto& bucket : hooks)
            {
                if (!bucket.token.valid())
                    continue;
                const auto error = bucket.endpoint->disconnect(bucket.endpoint->context, bucket.token);
                if (error == EEndpointMutationError::DISPATCH_ACTIVE || error == EEndpointMutationError::WRITER_ACTIVE)
                {
                    busy = true;
                    continue;
                }
                if (error != EEndpointMutationError::NONE && error != EEndpointMutationError::INVALID_TOKEN)
                    return lux::cxx::unexpected(EScriptSystemError::ENDPOINT_CONNECTION_FAILURE);
                bucket.token = {};
            }
            for (auto& bucket : events)
            {
                if (!bucket.token.valid())
                    continue;
                const auto error = bucket.endpoint->disconnect(bucket.endpoint->context, bucket.token);
                if (error == EEndpointMutationError::DISPATCH_ACTIVE || error == EEndpointMutationError::WRITER_ACTIVE)
                {
                    busy = true;
                    continue;
                }
                if (error != EEndpointMutationError::NONE && error != EEndpointMutationError::INVALID_TOKEN)
                    return lux::cxx::unexpected(EScriptSystemError::ENDPOINT_CONNECTION_FAILURE);
                bucket.token = {};
            }
            return busy ? lux::cxx::expected<void, EScriptSystemError>(
                              lux::cxx::unexpected(EScriptSystemError::ENDPOINT_BUSY))
                        : lux::cxx::expected<void, EScriptSystemError>{};
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
            auto* instance = instances.find(instanceKey(resume.instance));
            auto* continuation = continuations.find(continuationKey(resume.continuation));
            if (instance == nullptr || continuation == nullptr || continuation->instance != resume.instance ||
                continuation->waiting_on != resume.awaitable)
            {
                return {};
            }
            auto outcome = takeAwaitable(resume);
            if (!outcome)
                return {};
            if (instance->mount_slot >= mounts.size())
            {
                destroyContinuation(resume.continuation);
                return {};
            }
            auto& mount = mounts[instance->mount_slot];
            if (mount.state != EScriptMountState::ACTIVE || mount.instance != resume.instance)
            {
                destroyContinuation(resume.continuation);
                return {};
            }

            continuation->waiting_on = {};
            ScriptResumePacket packet{resume.awaitable, outcome->state, std::addressof(outcome->value), outcome->error};
            ScriptStepContext context{
                resume.instance,
                this,
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
            instance = instances.find(instanceKey(resume.instance));
            if (continuation == nullptr || instance == nullptr || instance->mount_slot >= mounts.size())
                return {};
            auto& current_mount = mounts[instance->mount_slot];
            if (current_mount.instance != resume.instance || current_mount.state != EScriptMountState::ACTIVE)
            {
                destroyContinuation(resume.continuation);
                return {};
            }
            auto& method = methods[continuation->method_slot];
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
                faultInvocation(current_mount, method.symbol, error);
                return lux::cxx::unexpected(error);
            }

            const auto status = result.error.status;
            destroyContinuation(resume.continuation);
            faultInvocation(current_mount, method.symbol, EScriptSystemError::INVOCATION_FAILURE, status);
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
            for (std::size_t index{mounts.size()}; index > 0U; --index)
            {
                releaseMount(
                    static_cast<std::uint32_t>(index - 1U),
                    EScriptMountState::INACTIVE,
                    true,
                    EScriptEndPlayReason::RUNTIME_STOPPED
                );
            }
            for (auto& mount : mounts)
                if (mount.id.valid())
                {
                    if (!mount.pending_scope)
                        ++pending_mounts;
                    mount.pending_scope = mount.status.submitted_scope;
                    if (const auto* entity = std::get_if<EntityScriptScope>(&*mount.pending_scope))
                        entity_associations.emplace(entity->self,
                            static_cast<std::uint32_t>(std::addressof(mount) - mounts.data()));
                }
            active_mount_count = 0U;
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

        std::array<ScriptBackendDescriptor, kBackendKindCount> backend_table{};
        for (const auto& backend : backends)
        {
            const auto index = backendIndex(backend.kind);
            const bool is_invalid_kind =
                backend.kind == lux::rdesc::Script::Kind::UNKNOWN || index >= backend_table.size();
            const bool is_invalid_functions = backend.createInstance == nullptr || backend.prepareMethod == nullptr ||
                                              backend.releaseMethod == nullptr || backend.destroyInstance == nullptr;
            if (is_invalid_kind || is_invalid_functions)
                return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);
            if (backend_table[index].kind != lux::rdesc::Script::Kind::UNKNOWN)
                return lux::cxx::unexpected(EScriptSystemError::DUPLICATE_BACKEND_KIND);
            backend_table[index] = backend;
        }

        try
        {
            auto state = std::make_unique<State>();
            static lux::cxx::ScopeIdSource<ScriptEventAdmissionScopeTag> event_scope_source;
            state->event_admission_scope = event_scope_source.acquire();
            state->simulation = std::addressof(simulation);
            state->capacity = capacity;
            state->registry = std::addressof(registry);
            state->clock = std::addressof(clock);
            state->limits = limits;
            state->delay_provider.owner = state.get();
            state->next_step_waits.reserve(limits.next_step_wait_capacity);
            state->simulation_delays.reserve(limits.simulation_delay_capacity);

            std::vector<PreparedScriptApiCapability> published_capabilities;
            published_capabilities.reserve(capabilities.size() + 1U);
            for (const auto& capability : capabilities)
            {
                if (!capability.contract.isValid() || capability.schema_hash == 0U || capability.dispatch == nullptr)
                    return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);
                published_capabilities.push_back({lux::script::ScriptApiContractId{capability.contract.name()},
                                                  capability.schema_hash,
                                                  capability.context,
                                                  capability.dispatch,
                                                  capability.schema_version,
                                                  capability.methods});
            }
            const auto delay_binding = lux::script::bindScriptAbility<DelayAbility>(state->delay_provider);
            const auto delay_publication = publishScriptAbility(delay_binding);
            if (!delay_publication.contract.isValid() || delay_publication.schema_hash == 0U ||
                delay_publication.dispatch == nullptr)
            {
                return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);
            }
            published_capabilities.push_back({
                lux::script::ScriptApiContractId{delay_publication.contract.name()},
                delay_publication.schema_hash,
                delay_publication.context,
                delay_publication.dispatch,
                delay_publication.schema_version,
                delay_publication.methods
            });
            std::sort(published_capabilities.begin(),
                      published_capabilities.end(),
                      [](const auto& left, const auto& right) noexcept {
                          return left.contract.hash() < right.contract.hash() ||
                                 (left.contract.hash() == right.contract.hash() &&
                                  left.contract.name() < right.contract.name());
                      });
            for (std::size_t index{1U}; index < published_capabilities.size(); ++index)
            {
                const auto& previous = published_capabilities[index - 1U].contract;
                const auto& current = published_capabilities[index].contract;
                if (previous.hash() != current.hash())
                    continue;
                const auto error = previous.name() == current.name()
                                       ? EScriptSystemError::SCRIPT_CAPABILITY_AMBIGUOUS_PROVIDER
                                       : EScriptSystemError::SCRIPT_CAPABILITY_ID_COLLISION;
                return lux::cxx::unexpected(error);
            }

            std::unordered_map<EndpointKey, std::uint32_t, EndpointKeyHash> hook_endpoint_index;
            std::unordered_map<EndpointKey, std::uint32_t, EndpointKeyHash> event_endpoint_index;
            hook_endpoint_index.reserve(hooks.size());
            event_endpoint_index.reserve(events.size());

            for (std::size_t index{}; index < hooks.size(); ++index)
            {
                if (index >= std::numeric_limits<std::uint32_t>::max())
                    return lux::cxx::unexpected(EScriptSystemError::CAPACITY_EXCEEDED);

                const auto described = simulation.findHookPoint(hooks[index].system, hooks[index].hook);
                const bool is_invalid_identity = !hooks[index].system.valid() || !hooks[index].hook.valid();
                const bool is_invalid_functions = hooks[index].connect == nullptr || hooks[index].disconnect == nullptr;
                const bool is_invalid_signature =
                    !described || !described.scriptCapable() ||
                    described.parameterCount() != hooks[index].signature.parameters.size() ||
                    !hooks[index].signature.returns.empty();
                if (is_invalid_identity || is_invalid_functions || is_invalid_signature)
                    return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);

                for (std::size_t parameter{}; parameter < described.parameterCount(); ++parameter)
                {
                    if (described.parameterAt(parameter) != hooks[index].signature.parameters[parameter])
                        return lux::cxx::unexpected(EScriptSystemError::SIGNATURE_MISMATCH);
                }
                const auto inserted =
                    hook_endpoint_index.emplace(EndpointKey{hooks[index].system.value, hooks[index].hook.value},
                                                static_cast<std::uint32_t>(index));
                if (!inserted.second)
                    return lux::cxx::unexpected(EScriptSystemError::DUPLICATE_ENDPOINT);
            }

            for (std::size_t index{}; index < events.size(); ++index)
            {
                if (index >= std::numeric_limits<std::uint32_t>::max())
                    return lux::cxx::unexpected(EScriptSystemError::CAPACITY_EXCEEDED);

                const auto described = simulation.findEvent(events[index].system, events[index].event);
                const auto& owned = events[index].payload_projection.owned_layout;
                const auto* builtin = lux::semantic::builtinLayout(owned.type_id);
                const bool is_invalid_builtin = builtin != nullptr &&
                    (builtin->canonical_name != owned.canonical_name || builtin->abi_kind != owned.abi_kind ||
                     builtin->size != owned.size || builtin->alignment != owned.alignment);
                const bool is_invalid_identity = !events[index].system.valid() || !events[index].event.valid();
                const bool is_invalid_functions =
                    events[index].connect == nullptr || events[index].disconnect == nullptr;
                const bool is_invalid_signature =
                    !described || !described.dispatchHook().scriptCapable() ||
                    described.route() != events[index].route ||
                    described.payloadType() != events[index].payload_type.type_id ||
                    described.payloadSchemaName() != events[index].payload_type.canonical_name ||
                    events[index].payload_type.pass != lux::semantic::EValuePass::CONST_REF ||
                    owned.type_id != events[index].payload_type.type_id ||
                    owned.canonical_name != events[index].payload_type.canonical_name || owned.abi_kind == 0U ||
                    owned.size == 0U || owned.alignment == 0U ||
                    (owned.alignment & (owned.alignment - 1U)) != 0U || is_invalid_builtin;
                if (is_invalid_identity || is_invalid_functions || is_invalid_signature)
                    return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);

                const auto inserted =
                    event_endpoint_index.emplace(EndpointKey{events[index].system.value, events[index].event.value},
                                                 static_cast<std::uint32_t>(index));
                if (!inserted.second)
                    return lux::cxx::unexpected(EScriptSystemError::DUPLICATE_ENDPOINT);
            }

            state->artifacts = artifacts;
            state->host = host;
            state->real_delay = real_delay;
            state->backends = backend_table;
            state->hook_endpoints.assign(hooks.begin(), hooks.end());
            state->event_endpoints.assign(events.begin(), events.end());
            state->hook_endpoint_index = std::move(hook_endpoint_index);
            state->event_endpoint_index = std::move(event_endpoint_index);
            state->published_capabilities = std::move(published_capabilities);
            state->failures.reserve(limits.failure_capacity);
            state->instances.reserve(limits.instance_capacity);
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
        for (std::size_t mount_slot{}; mount_slot < state_->mounts.size(); ++mount_slot)
        {
            auto initialized = state_->initializeMount(static_cast<std::uint32_t>(mount_slot));
            if (!initialized)
            {
                const auto rolled_back = state_->rollbackPrepare();
                return rolled_back ? initialized : rolled_back;
            }
            if (state_->mounts[mount_slot].state == EScriptMountState::INITIALIZED)
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
            if (state_->mounts[mount_slot].state != EScriptMountState::INITIALIZED)
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
            if (state_->mounts[mount_slot].state == EScriptMountState::INITIALIZED)
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
        if (state_->endpoint_dispatch_depth != 0U || state_->user_invocation_depth != 0U ||
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
            auto& mount = state_->mounts[mount_slot];
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
            auto& mount = state_->mounts[mount_slot];
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
                return state_->mounts[left].admission_order < state_->mounts[right].admission_order;
            });
        state_->lifecycle_initialized.clear();
        for (const auto mount_slot : state_->lifecycle_candidates)
        {
            const auto initialized = state_->initializeMount(mount_slot);
            if (initialized)
            {
                if (state_->mounts[mount_slot].state == EScriptMountState::INITIALIZED)
                    state_->lifecycle_initialized.push_back(mount_slot);
                continue;
            }

            const auto error = initialized.error();
            if (!first_error)
                first_error = error;

            auto& failed_mount = state_->mounts[mount_slot];
            failed_mount.state = EScriptMountState::FAULTED;
            failed_mount.status.submission_state = EScriptMountSubmissionState::REJECTED;
            failed_mount.status.submission_error = error;
            failed_mount.unconsumed_result = true;
            state_->markStatus(failed_mount);
            state_->recordFailure(error, failed_mount);
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
            auto& mount = state_->mounts[mount_slot];
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
                state_->recordFailure(error, mount);
                if (!first_error)
                    first_error = error;
            }
        }
        for (const auto mount_slot : state_->lifecycle_initialized)
        {
            if (state_->mounts[mount_slot].state == EScriptMountState::INITIALIZED &&
                state_->mounts[mount_slot].gameplay_lifetime_started)
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
        if (!execution || state_->endpoint_dispatch_depth != 0U || state_->user_invocation_depth != 0U)
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
            state_->endpoint_dispatch_depth != 0U || state_->user_invocation_depth != 0U;
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
        if (!execution || state_->user_invocation_depth != 0U || state_->endpoint_dispatch_depth != 0U)
            return lux::cxx::unexpected(EScriptSystemError::ENDPOINT_BUSY);
        const auto slot = state_->findMount(id);
        if (!slot)
            return std::optional<ScriptMountStatus>{};
        return std::optional<ScriptMountStatus>{state_->mounts[*slot].status};
    }

    lux::cxx::expected<ScriptMountStatusCollection, EScriptSystemError>
    ScriptSystem::collectMountStatusChanges(std::span<ScriptMountStatus> output) noexcept
    {
        if (!state_)
            return lux::cxx::unexpected(EScriptSystemError::SHUT_DOWN);
        State::ExecutionOwnerScope execution{*state_};
        if (!execution || state_->user_invocation_depth != 0U || state_->endpoint_dispatch_depth != 0U)
            return lux::cxx::unexpected(EScriptSystemError::ENDPOINT_BUSY);
        auto& changes = state_->status_changes;
        const auto count = (std::min)(output.size(), changes.values.size());
        for (std::size_t index{}; index < count; ++index)
        {
            const auto slot = changes.values[index];
            auto& mount = state_->mounts[slot];
            output[index] = mount.status;
            mount.unconsumed_result = false;
            changes.present[slot] = 0U;
        }
        changes.values.erase(changes.values.begin(), changes.values.begin() + count);
        return ScriptMountStatusCollection{count, changes.values.size()};
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
        if (state_->user_invocation_depth != 0U || state_->result_write_pins != 0U)
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
        for (std::size_t index{}; index < state_->mounts.size(); ++index)
        {
            auto& mount = state_->mounts[index];
            if (mount.state == EScriptMountState::INACTIVE && !mount.backend_instance)
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
        state_->instances.clear();
        state_->published_capabilities.clear();
        state_->entity_associations.clear();
        state_->awaitables.clear();
        state_->resumes.clear();
        for (auto& mount : state_->mounts)
        {
            if (mount.pending_scope)
            {
                mount.pending_scope.reset();
                --state_->pending_mounts;
                mount.status.submission_state = EScriptMountSubmissionState::CANCELLED;
                mount.status.submission_error = EScriptSystemError::SHUT_DOWN;
                mount.unconsumed_result = true;
                state_->markStatus(mount);
            }
        }
        state_->prepare_state = EPrepareState::SHUT_DOWN;
        return {};
    }

    std::size_t ScriptSystem::activeInstanceCount() const noexcept
    {
        return state_ ? state_->active_mount_count : 0U;
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
        result.configured_mounts = state_->configured_mounts;
        result.pending_mounts = state_->pending_mounts;
        result.mount_backing_bytes = state_->mounts.capacity() * sizeof(State::RuntimeMount);
        result.method_backing_bytes = state_->methods.capacity() * sizeof(State::PreparedMethod);
        result.binding_backing_bytes = state_->bindings.capacity() * sizeof(State::RuntimeBinding) +
            state_->binding_descriptions.capacity() * sizeof(ScriptBindingDescription);
        result.mount_feedback_backing_bytes = state_->status_changes.values.capacity() * sizeof(std::uint32_t) +
            state_->status_changes.present.capacity() * sizeof(std::uint8_t);
        result.active_instances = state_->active_mount_count;
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
