#include <lux/engine/simulation/ScriptSystem.hpp>
#include <lux/engine/simulation/detail/DenseEntityHandlerStorage.hpp>
#include <lux/engine/simulation/abilities/DelayAbility.hpp>
#include "DelayAbility.ability.generated.hpp"
#include <lux/engine/simulation/scripting/ScriptLifecycle.hpp>

#include <lux/cxx/container/SlotMap.hpp>

#include <entt/signal/sigh.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lux::simulation::script
{
    namespace
    {
        constexpr std::size_t kBackendKindCount{7U};
        constexpr std::uint32_t kInvalidMethodSlot = std::numeric_limits<std::uint32_t>::max();

        enum class EMountState : std::uint8_t
        {
            INACTIVE,
            CONSTRUCTING,
            INITIALIZED,
            ACTIVE,
            RETIRING,
            FAULTED,
        };

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

        [[nodiscard]] bool sameType(const lux::rdesc::ScriptValueType& script_type,
                                    const lux::semantic::Type& endpoint_type) noexcept
        {
            return script_type.type_id == endpoint_type.type_id &&
                   script_type.canonical_name == endpoint_type.canonical_name && script_type.pass == endpoint_type.pass;
        }

        [[nodiscard]] bool sameHookSignature(const lux::rdesc::ScriptFunction& function,
                                             lux::semantic::SignatureView signature) noexcept
        {
            if (!function.returns.empty() || function.args.size() != signature.parameters.size())
                return false;

            for (std::size_t index{}; index < function.args.size(); ++index)
            {
                if (!sameType(function.args[index], signature.parameters[index]))
                    return false;
            }
            return true;
        }

        [[nodiscard]] bool sameEventSignature(const lux::rdesc::ScriptFunction& function,
                                              const lux::semantic::Type& payload) noexcept
        {
            return function.returns.empty() && function.args.size() == 1U && sameType(function.args.front(), payload);
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

        struct NextStepWait final
        {
            std::uint64_t target_step{};
            std::uint64_t sequence{};
            lux::script::ScriptAbilityCompletion<void> completion;
        };

        struct NextStepLater final
        {
            [[nodiscard]] bool operator()(const NextStepWait& left, const NextStepWait& right) const noexcept
            {
                return left.target_step > right.target_step ||
                    (left.target_step == right.target_step && left.sequence > right.sequence);
            }
        };

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
            lux::script::BoundScriptCall call;
            BoundScriptStepCall step;
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
            const ScriptMountDescription* authored{};
            std::size_t binding_first{};
            std::size_t binding_count{};
            std::size_t method_first{};
            std::size_t method_count{};
            std::uint32_t begin_play_method{kInvalidMethodSlot};
            std::uint32_t end_play_method{kInvalidMethodSlot};
            ScriptInstanceScope scope;
            ScriptBehavior behavior;
            ScriptInstanceId instance;
            std::vector<PreparedScriptApiCapability> capabilities;
            ResolvedScriptArtifact artifact;
            const ScriptBackendDescriptor* backend{};
            ScriptBackendInstance backend_instance;
            ecs::Entity entity{ecs::NullEntity};
            EMountState state{EMountState::INACTIVE};
            bool active_counted{};
            bool retirement_queued{};
            bool gameplay_lifetime_started{};
            EScriptEndPlayReason pending_end_reason{EScriptEndPlayReason::OBJECT_UNMATERIALIZED};
        };

        struct RetirementRecord final
        {
            std::uint32_t mount_slot{};
            EScriptEndPlayReason reason{EScriptEndPlayReason::OBJECT_UNMATERIALIZED};
            EMountState final_state{EMountState::INACTIVE};
            bool invoke_end_play{};
        };

        struct InstanceTag;
        struct ContinuationTag;
        struct AwaitableTag;

        struct InstanceRecord final
        {
            ScriptInstanceId id;
            std::uint32_t mount_slot{};
            std::size_t active_continuations{};
            ScriptContinuationId first_continuation;
            ScriptAwaitableId first_awaitable;
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

        struct AwaitableRecord final
        {
            ScriptAwaitableId id;
            ScriptInstanceId instance;
            ScriptContinuationId continuation;
            EScriptAwaitableState state{EScriptAwaitableState::PENDING};
            std::optional<lux::rdesc::ScriptValueType> result_type;
            ScriptOwnedResumeValue value;
            ScriptStepError error;
            bool resume_enqueued{};
            ScriptAwaitableId instance_previous;
            ScriptAwaitableId instance_next;
        };

        using InstanceStorage = lux::cxx::SlotMap<InstanceRecord, InstanceTag>;
        using InstanceKey = typename InstanceStorage::key_type;
        using ContinuationStorage = lux::cxx::SlotMap<ContinuationRecord, ContinuationTag>;
        using ContinuationKey = typename ContinuationStorage::key_type;
        using AwaitableStorage = lux::cxx::SlotMap<AwaitableRecord, AwaitableTag>;
        using AwaitableKey = typename AwaitableStorage::key_type;

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

        struct AwaitableIngress final
        {
            std::mutex mutex;
            AwaitableStorage awaitables;
            ResumeRing resumes;
            std::size_t awaitable_capacity{};
            std::size_t max_payload_bytes{};
            bool stopping{};

            [[nodiscard]] static bool validValueType(const lux::rdesc::ScriptValueType& type) noexcept
            {
                return lux::rdesc::detail::validScriptValueType(type) && type.pass == lux::semantic::EValuePass::VALUE;
            }

            [[nodiscard]] bool validOutcome(const AwaitableRecord& record,
                                            EScriptAwaitableState state,
                                            const ScriptOwnedResumeValue& value,
                                            ScriptStepError error) const noexcept
            {
                if (state == EScriptAwaitableState::READY)
                {
                    if (error.valid() || value.bytes.size() > max_payload_bytes ||
                        value.type.has_value() != record.result_type.has_value())
                    {
                        return false;
                    }
                    if (!record.result_type)
                        return value.bytes.empty();
                    return value.type == record.result_type && value.bytes.size() == record.result_type->size;
                }
                return state == EScriptAwaitableState::FAILED && error.valid() && !value.type && value.bytes.empty();
            }

            [[nodiscard]] lux::cxx::expected<void, EScriptAwaitableCompletionError> complete(
                ScriptInstanceId instance,
                ScriptAwaitableId awaitable,
                EScriptAwaitableState state,
                ScriptOwnedResumeValue value,
                ScriptStepError error) noexcept
            {
                std::lock_guard lock{mutex};
                if (stopping)
                    return lux::cxx::unexpected(EScriptAwaitableCompletionError::STOPPING);
                auto* record = awaitables.find(awaitableKey(awaitable));
                if (record == nullptr || record->instance != instance)
                    return lux::cxx::unexpected(EScriptAwaitableCompletionError::INVALID_ID);
                if (record->state != EScriptAwaitableState::PENDING)
                    return lux::cxx::unexpected(EScriptAwaitableCompletionError::ALREADY_TERMINAL);
                if (!validOutcome(*record, state, value, error))
                    return lux::cxx::unexpected(EScriptAwaitableCompletionError::INVALID_VALUE);
                if (record->continuation.valid() && resumes.count >= resumes.records.size())
                    return lux::cxx::unexpected(EScriptAwaitableCompletionError::RESUME_QUEUE_FULL);

                record->state = state;
                record->value = std::move(value);
                record->error = error;
                if (record->continuation.valid())
                {
                    static_cast<void>(resumes.push({record->instance, record->continuation, record->id}));
                    record->resume_enqueued = true;
                }
                return {};
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

            [[nodiscard]] bool active(ScriptInstanceId instance, ScriptAwaitableId awaitable) noexcept
            {
                std::lock_guard lock{mutex};
                if (stopping)
                    return false;
                const auto* record = awaitables.find(awaitableKey(awaitable));
                return record != nullptr && record->instance == instance &&
                    record->state == EScriptAwaitableState::PENDING;
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
        const ScriptSystemDescription* description{};
        ecs::Registry* registry{};
        const SimulationClock* clock{};
        ScriptRuntimeLimits limits;
        ScriptArtifactResolver artifacts;
        WorldObjectResolver world;
        ScriptHostApi host;
        ScriptRealDelayEndpoint real_delay;
        std::array<ScriptBackendDescriptor, kBackendKindCount> backends;
        std::vector<ScriptHookEndpointDescriptor> hook_endpoints;
        std::vector<ScriptEventEndpointDescriptor> event_endpoints;
        std::unordered_map<EndpointKey, std::uint32_t, EndpointKeyHash> hook_endpoint_index;
        std::unordered_map<EndpointKey, std::uint32_t, EndpointKeyHash> event_endpoint_index;
        std::vector<PreparedScriptApiCapability> published_capabilities;
        std::vector<RuntimeMount> mounts;
        std::vector<RuntimeBinding> bindings;
        std::vector<PreparedMethod> methods;
        std::vector<HookBucket> hooks;
        std::vector<EventBucket> events;
        std::vector<ScriptSystemFailure> failures;
        InstanceStorage instances;
        ContinuationStorage continuations;
        std::shared_ptr<AwaitableIngress> ingress;
        std::vector<std::uint32_t> retirement_queue;
        SparseMountQueue dirty_current;
        SparseMountQueue dirty_processing;
        std::vector<std::uint32_t> lifecycle_candidates;
        std::vector<std::uint32_t> lifecycle_initialized;
        std::vector<RetirementRecord> lifecycle_retirements;
        DelayProvider delay_provider{this};
        std::mutex delay_mutex;
        std::vector<NextStepWait> next_step_waits;
        std::vector<SimulationDelayWait> simulation_delays;
        std::uint64_t delay_sequence{};
        entt::connection constructed;
        entt::connection updated;
        entt::connection destroyed;
        std::size_t active_mount_count{};
        bool suppress_attachment_signal{};
        bool stopping{};
        EPrepareState prepare_state{EPrepareState::CREATED};

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

            std::lock_guard lock{delay_mutex};
            if (next_step_waits.size() >= limits.next_step_wait_capacity)
            {
                std::erase_if(next_step_waits, [](const NextStepWait& wait) noexcept {
                    return !wait.completion.active();
                });
                std::make_heap(next_step_waits.begin(), next_step_waits.end(), NextStepLater{});
            }
            if (next_step_waits.size() >= limits.next_step_wait_capacity)
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
                next_step_waits.push_back({current.step_index + 1U, delay_sequence++, std::move(completion)});
                std::push_heap(next_step_waits.begin(), next_step_waits.end(), NextStepLater{});
                return {};
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(lux::script::ScriptAbilityOperationError{
                    static_cast<std::int32_t>(EScriptDelayStatus::ALLOCATION_FAILURE)
                });
            }
        }

        [[nodiscard]] bool promoteNextStepWaits() noexcept
        {
            const auto current = clock->snapshot();
            for (;;)
            {
                std::optional<NextStepWait> ready;
                {
                    std::lock_guard lock{delay_mutex};
                    if (next_step_waits.empty() || next_step_waits.front().target_step > current.step_index)
                        return true;
                    std::pop_heap(next_step_waits.begin(), next_step_waits.end(), NextStepLater{});
                    ready.emplace(std::move(next_step_waits.back()));
                    next_step_waits.pop_back();
                }

                auto completed = ready->completion.success();
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
                    std::lock_guard lock{delay_mutex};
                    next_step_waits.push_back(std::move(*ready));
                    std::push_heap(next_step_waits.begin(), next_step_waits.end(), NextStepLater{});
                    return true;
                }
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

            std::lock_guard lock{delay_mutex};
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
                    std::lock_guard lock{delay_mutex};
                    if (simulation_delays.empty())
                        return true;
                    const auto& next = simulation_delays.front();
                    if (next.deadline > current.elapsed || next.minimum_step > current.step_index)
                        return true;
                    std::pop_heap(simulation_delays.begin(), simulation_delays.end(), SimulationDelayLater{});
                    ready.emplace(std::move(simulation_delays.back()));
                    simulation_delays.pop_back();
                }

                auto completed = ready->completion.success();
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
                    std::lock_guard lock{delay_mutex};
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

        [[nodiscard]] lux::cxx::expected<ScriptAwaitableRegistration, EScriptAwaitableCreateError> createAwaitable(
            ScriptInstanceId instance,
            std::optional<lux::rdesc::ScriptValueType> result_type) noexcept
        {
            auto* owner = instances.find(instanceKey(instance));
            if (owner == nullptr)
                return lux::cxx::unexpected(EScriptAwaitableCreateError::INVALID_INSTANCE);
            if (result_type &&
                (!AwaitableIngress::validValueType(*result_type) || result_type->size > ingress->max_payload_bytes))
            {
                return lux::cxx::unexpected(EScriptAwaitableCreateError::INVALID_RESULT_TYPE);
            }

            std::lock_guard lock{ingress->mutex};
            if (ingress->stopping)
                return lux::cxx::unexpected(EScriptAwaitableCreateError::STOPPING);
            if (ingress->awaitables.size() >= ingress->awaitable_capacity)
                return lux::cxx::unexpected(EScriptAwaitableCreateError::CAPACITY_EXCEEDED);
            auto inserted = ingress->awaitables.tryEmplace(
                AwaitableRecord{{}, instance, {}, EScriptAwaitableState::PENDING, std::move(result_type)});
            if (!inserted)
                return lux::cxx::unexpected(EScriptAwaitableCreateError::ALLOCATION_FAILURE);
            const auto id = awaitableId(*inserted);
            auto& record = ingress->awaitables[*inserted];
            record.id = id;
            record.instance_next = owner->first_awaitable;
            if (record.instance_next.valid())
            {
                auto* next = ingress->awaitables.find(awaitableKey(record.instance_next));
                if (next == nullptr)
                    std::terminate();
                next->instance_previous = id;
            }
            owner->first_awaitable = id;
            return ScriptAwaitableRegistration{id,
                                               ScriptAwaitableCompletion{std::static_pointer_cast<void>(ingress),
                                                                         ingress.get(),
                                                                         &AwaitableIngress::completeErased,
                                                                         &AwaitableIngress::activeErased,
                                                                         instance,
                                                                         id}};
        }

        [[nodiscard]] static lux::cxx::expected<ScriptAwaitableRegistration, EScriptAwaitableCreateError>
        createAwaitableErased(void* context,
                              ScriptInstanceId instance,
                              std::optional<lux::rdesc::ScriptValueType> result_type) noexcept
        {
            return static_cast<State*>(context)->createAwaitable(instance, std::move(result_type));
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptSystemError> attachWaiter(
            ScriptAwaitableId awaitable,
            ScriptInstanceId instance,
            ScriptContinuationId continuation) noexcept
        {
            std::lock_guard lock{ingress->mutex};
            auto* record = ingress->awaitables.find(awaitableKey(awaitable));
            if (record == nullptr || record->instance != instance || record->continuation.valid())
                return lux::cxx::unexpected(EScriptSystemError::INVOCATION_FAILURE);
            if (record->state != EScriptAwaitableState::PENDING &&
                ingress->resumes.count >= ingress->resumes.records.size())
            {
                return lux::cxx::unexpected(EScriptSystemError::RESUME_QUEUE_FULL);
            }
            record->continuation = continuation;
            if (record->state != EScriptAwaitableState::PENDING)
            {
                static_cast<void>(ingress->resumes.push({instance, continuation, awaitable}));
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
                auto* previous = ingress->awaitables.find(awaitableKey(record.instance_previous));
                if (previous != nullptr)
                    previous->instance_next = record.instance_next;
            }
            else if (owner != nullptr && owner->first_awaitable == record.id)
            {
                owner->first_awaitable = record.instance_next;
            }
            if (record.instance_next.valid())
            {
                auto* next = ingress->awaitables.find(awaitableKey(record.instance_next));
                if (next != nullptr)
                    next->instance_previous = record.instance_previous;
            }
            record.instance_previous = {};
            record.instance_next = {};
        }

        [[nodiscard]] bool eraseAwaitableLocked(ScriptAwaitableId id) noexcept
        {
            auto* record = ingress->awaitables.find(awaitableKey(id));
            if (record == nullptr)
                return false;
            unlinkAwaitableOwnership(*record);
            return ingress->awaitables.erase(awaitableKey(id));
        }

        [[nodiscard]] std::optional<AwaitableOutcome> takeAwaitable(ResumeRecord resume) noexcept
        {
            std::lock_guard lock{ingress->mutex};
            auto* record = ingress->awaitables.find(awaitableKey(resume.awaitable));
            if (record == nullptr || record->instance != resume.instance ||
                record->continuation != resume.continuation ||
                (record->state != EScriptAwaitableState::READY && record->state != EScriptAwaitableState::FAILED))
            {
                return std::nullopt;
            }
            AwaitableOutcome outcome{record->state, std::move(record->value), record->error};
            static_cast<void>(eraseAwaitableLocked(resume.awaitable));
            return outcome;
        }

        void cancelAwaitables(ScriptInstanceId instance, ScriptAwaitableId first) noexcept
        {
            std::lock_guard lock{ingress->mutex};
            auto current = first;
            while (current.valid())
            {
                auto* record = ingress->awaitables.find(awaitableKey(current));
                if (record == nullptr || record->instance != instance)
                    std::terminate();
                const auto next = record->instance_next;
                static_cast<void>(eraseAwaitableLocked(current));
                current = next;
            }
        }

        void discardAwaitable(ScriptInstanceId instance, ScriptAwaitableId awaitable) noexcept
        {
            if (!awaitable.valid())
                return;
            std::lock_guard lock{ingress->mutex};
            auto* record = ingress->awaitables.find(awaitableKey(awaitable));
            if (record != nullptr && record->instance == instance)
                static_cast<void>(eraseAwaitableLocked(awaitable));
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
            std::lock_guard lock{ingress->mutex};
            return ingress->resumes.pop();
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
                current = next;
            }
        }

        void invalidateInstance(RuntimeMount& mount) noexcept
        {
            const auto instance = mount.instance;
            if (!instance.valid())
                return;
            const auto* record = instances.find(instanceKey(instance));
            if (record == nullptr)
                std::terminate();
            const auto first_awaitable = record->first_awaitable;
            const auto first_continuation = record->first_continuation;
            static_cast<void>(instances.erase(instanceKey(instance)));
            cancelAwaitables(instance, first_awaitable);
            destroyContinuations(instance, first_continuation);
            mount.instance = {};
        }

        void faultInvocation(RuntimeMount& mount,
                             lux::script::ScriptSymbolId symbol,
                             EScriptSystemError error,
                             std::int32_t status = 0) noexcept
        {
            mount.state = EMountState::FAULTED;
            mount.pending_end_reason = EScriptEndPlayReason::FAULTED;
            deactivate(mount);
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

        [[nodiscard]] lux::cxx::expected<void, EScriptSystemError> buildLayout() noexcept
        {
            try
            {
                const auto authored_mounts = description->mounts();
                std::size_t binding_capacity{};
                for (const auto& mount : authored_mounts)
                    binding_capacity += mount.enabled ? mount.bindings.size() : 0U;
                if (authored_mounts.size() > (std::numeric_limits<std::size_t>::max() - binding_capacity) / 2U)
                    return lux::cxx::unexpected(EScriptSystemError::CAPACITY_EXCEEDED);
                const std::size_t method_capacity = binding_capacity + authored_mounts.size() * 2U;

                mounts.clear();
                mounts.resize(authored_mounts.size());
                bindings.clear();
                bindings.reserve(binding_capacity);
                methods.clear();
                methods.reserve(method_capacity);
                hooks.clear();
                hooks.resize(hook_endpoints.size());
                for (std::size_t index{}; index < hooks.size(); ++index)
                {
                    hooks[index].owner = this;
                    hooks[index].endpoint = std::addressof(hook_endpoints[index]);
                }
                events.clear();
                events.resize(event_endpoints.size());
                for (std::size_t index{}; index < events.size(); ++index)
                {
                    events[index].owner = this;
                    events[index].endpoint = std::addressof(event_endpoints[index]);
                }

                for (std::size_t mount_slot{}; mount_slot < authored_mounts.size(); ++mount_slot)
                {
                    const auto& authored = authored_mounts[mount_slot];
                    auto& mount = mounts[mount_slot];
                    mount.authored = std::addressof(authored);
                    mount.binding_first = bindings.size();
                    mount.method_first = methods.size();
                    if (!authored.enabled)
                        continue;

                    std::unordered_map<lux::script::ScriptSymbolId, std::uint32_t> method_index;
                    method_index.reserve(authored.bindings.size());

                    for (const auto& binding : authored.bindings)
                    {
                        RuntimeBinding runtime;
                        const auto existing_method = method_index.find(binding.symbol);
                        if (existing_method != method_index.end())
                        {
                            runtime.method_slot = existing_method->second;
                        }
                        else
                        {
                            if (methods.size() >= std::numeric_limits<std::uint32_t>::max())
                                return lux::cxx::unexpected(EScriptSystemError::CAPACITY_EXCEEDED);
                            runtime.method_slot = static_cast<std::uint32_t>(methods.size());
                            methods.push_back({binding.symbol, {}, {}, {}, true});
                            method_index.emplace(binding.symbol, runtime.method_slot);
                            ++mount.method_count;
                        }
                        if (const auto* target = std::get_if<HookScriptTarget>(&binding.target))
                        {
                            const auto endpoint = findHookEndpoint(*target);
                            if (!endpoint)
                                return lux::cxx::unexpected(EScriptSystemError::SCRIPT_ENDPOINT_NOT_FOUND);

                            runtime.kind = EBindingKind::HOOK;
                            runtime.bucket_slot = *endpoint;
                            ++hooks[runtime.bucket_slot].handler_capacity;
                        }
                        else
                        {
                            const auto event_target = std::get<EventScriptTarget>(binding.target);
                            const auto endpoint = findEventEndpoint(event_target);
                            if (!endpoint)
                                return lux::cxx::unexpected(EScriptSystemError::SCRIPT_ENDPOINT_NOT_FOUND);

                            runtime.kind = EBindingKind::EVENT;
                            runtime.bucket_slot = *endpoint;
                            ++events[runtime.bucket_slot].handler_capacity;
                        }
                        bindings.push_back(runtime);
                        ++mount.binding_count;
                    }
                    methods.push_back({});
                    methods.push_back({});
                    mount.method_count += 2U;
                }

                for (auto& bucket : hooks)
                    bucket.handlers.reserve(bucket.handler_capacity);
                for (auto& bucket : events)
                {
                    const auto prepared = bucket.handlers.prepare(bucket.handler_capacity);
                    if (prepared == EEndpointMutationError::ALLOCATION_FAILURE)
                        return lux::cxx::unexpected(EScriptSystemError::ALLOCATION_FAILURE);
                }

                dirty_current.prepare(mounts.size());
                dirty_processing.prepare(mounts.size());
                retirement_queue.clear();
                retirement_queue.reserve(mounts.size());
                lifecycle_candidates.clear();
                lifecycle_candidates.reserve(mounts.size());
                lifecycle_initialized.clear();
                lifecycle_initialized.reserve(mounts.size());
                lifecycle_retirements.clear();
                lifecycle_retirements.reserve(mounts.size());
                return {};
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(EScriptSystemError::ALLOCATION_FAILURE);
            }
        }

        void recordFailure(EScriptSystemError error,
                           RuntimeMount& mount,
                           lux::script::ScriptSymbolId symbol = 0U,
                           std::int32_t status = 0) noexcept
        {
            if (failures.size() < limits.failure_capacity)
                failures.push_back({error, mount.authored->id, symbol, status});
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
            if (stopping || mount.state != EMountState::ACTIVE)
                return;

            auto& method = methods[handler.method_slot];
            if (hook_invocation && method.active_hook.valid() &&
                continuations.find(continuationKey(method.active_hook)) != nullptr)
            {
                return;
            }
            if (method.step)
            {
                ScriptBackendContinuation continuation;
                ScriptStepContext context{
                    mount.instance,
                    this,
                    &State::createAwaitableErased,
                    &State::discardAwaitableErased
                };
                const auto result = method.step.invoke(method.step.context, frame, context, continuation);
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

            frame.user_context = method.call.context;
            const auto status = method.call.invoke(&frame);
            if (status == 0)
                return;
            faultInvocation(mount, method.symbol, EScriptSystemError::INVOCATION_FAILURE, status);
        }

        static void invokeHookLane(void* context, lux_script_call_frame& frame) noexcept
        {
            auto& bucket = *static_cast<HookBucket*>(context);
            for (auto& handler : bucket.handlers.values())
                bucket.owner->invoke(handler, frame, true);
        }

        static void dispatchEvent(void* context, ecs::Entity entity, lux_script_call_frame& frame) noexcept
        {
            auto& bucket = *static_cast<EventBucket*>(context);
            const auto invoke = [&bucket, &frame](Handler& handler) noexcept {
                bucket.owner->invoke(handler, frame, false);
            };
            if (bucket.endpoint->route == EEventRoute::SIMULATION_BROADCAST)
            {
                bucket.handlers.forEachAll(invoke);
                return;
            }
            bucket.handlers.forEachTarget(entity, invoke);
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
            frame.user_context = method.call.context;
            return method.call.invoke(&frame);
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptSystemError> beginPlayMount(std::uint32_t mount_slot) noexcept
        {
            auto& mount = mounts[mount_slot];
            if (mount.state != EMountState::INITIALIZED)
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
            EMountState final_state,
            bool remove_attachment
        ) noexcept
        {
            auto& mount = mounts[mount_slot];
            const bool invoke_end_play = mount.gameplay_lifetime_started;
            mount.gameplay_lifetime_started = false;
            mount.state = EMountState::RETIRING;
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
            if (retirement.invoke_end_play)
                endPlayMount(mount, retirement.reason);
            if (mount.backend != nullptr && mount.backend_instance)
            {
                const auto method_end = mount.method_first + mount.method_count;
                for (std::size_t index{method_end}; index > mount.method_first; --index)
                {
                    auto& method = methods[index - 1U];
                    if (method.step)
                    {
                        mount.backend->releaseStepMethod(mount.backend->context, mount.backend_instance, method.step);
                        method.step = {};
                    }
                    if (!method.call)
                        continue;
                    mount.backend->releaseMethod(mount.backend->context, mount.backend_instance, method.call);
                    method.call = {};
                }
                mount.backend->destroyInstance(mount.backend->context, mount.backend_instance);
            }
            mount.capabilities.clear();
            if (mount.artifact.lease != nullptr && mount.artifact.release != nullptr)
                mount.artifact.release(mount.artifact.lease);

            resetMountRuntime(mount);
            mount.state = retirement.final_state;
        }

        void releaseMount(
            std::uint32_t mount_slot,
            EMountState final_state,
            bool remove_attachment,
            EScriptEndPlayReason reason = EScriptEndPlayReason::OBJECT_UNMATERIALIZED
        ) noexcept
        {
            finishRetirement(beginRetirement(mount_slot, reason, final_state, remove_attachment));
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptSystemError> initializeMount(
            std::uint32_t mount_slot,
            std::optional<ecs::Entity> forced_entity = std::nullopt) noexcept
        {
            auto& mount = mounts[mount_slot];
            if (!mount.authored->enabled || mount.state == EMountState::FAULTED)
                return {};
            if (mount.state == EMountState::ACTIVE || mount.state == EMountState::INITIALIZED)
                return {};

            mount.state = EMountState::CONSTRUCTING;
            if (std::holds_alternative<SimulationScriptMount>(mount.authored->scope))
            {
                mount.scope = SimulationScriptScope{};
                mount.entity = ecs::NullEntity;
            }
            else
            {
                ecs::Entity entity{ecs::NullEntity};
                const auto& object = std::get<EntityScriptMount>(mount.authored->scope).object;
                const bool resolved = forced_entity.has_value() ||
                                      (world.resolve != nullptr && world.resolve(world.context, object, entity));
                if (forced_entity)
                    entity = *forced_entity;
                if (!resolved || entity == ecs::NullEntity || !registry->valid(entity))
                {
                    mount.state = EMountState::INACTIVE;
                    return lux::cxx::unexpected(EScriptSystemError::WORLD_OBJECT_NOT_RESOLVED);
                }
                mount.scope = EntityScriptScope{entity};
                mount.entity = entity;
            }
            mount.behavior.attach(mount.scope, host);

            if (!artifacts.resolve(artifacts.context, mount.authored->asset, mount.artifact))
            {
                resetMountRuntime(mount);
                mount.state = EMountState::INACTIVE;
                return lux::cxx::unexpected(EScriptSystemError::ASSET_NOT_RESIDENT);
            }
            if (mount.artifact.artifact == nullptr)
            {
                releaseMount(mount_slot, EMountState::INACTIVE, false);
                return lux::cxx::unexpected(EScriptSystemError::INVALID_ASSET);
            }

            mount.backend = backend(mount.artifact.artifact->description().kind());
            if (mount.backend == nullptr)
            {
                releaseMount(mount_slot, EMountState::INACTIVE, false);
                return lux::cxx::unexpected(EScriptSystemError::BACKEND_NOT_AVAILABLE);
            }

            const auto lifecycle = mount.artifact.artifact->description().lifecycle;
            const auto begin_method = claimLifecycleMethod(mount, lifecycle.begin_play);
            const auto end_method = claimLifecycleMethod(mount, lifecycle.end_play);
            if (!begin_method || !end_method)
            {
                const auto error = !begin_method ? begin_method.error() : end_method.error();
                releaseMount(mount_slot, EMountState::INACTIVE, false);
                return lux::cxx::unexpected(error);
            }
            mount.begin_play_method = *begin_method;
            mount.end_play_method = *end_method;
            if (mount.begin_play_method != kInvalidMethodSlot)
            {
                const auto* function = mount.artifact.artifact->findExport(lifecycle.begin_play);
                if (function == nullptr || !validBeginPlaySignature(*function))
                {
                    releaseMount(mount_slot, EMountState::INACTIVE, false);
                    return lux::cxx::unexpected(EScriptSystemError::SIGNATURE_MISMATCH);
                }
            }
            if (mount.end_play_method != kInvalidMethodSlot)
            {
                const auto* function = mount.artifact.artifact->findExport(lifecycle.end_play);
                if (function == nullptr || !validEndPlaySignature(*function))
                {
                    releaseMount(mount_slot, EMountState::INACTIVE, false);
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
                        releaseMount(mount_slot, EMountState::INACTIVE, false);
                        return lux::cxx::unexpected(EScriptSystemError::SCRIPT_CAPABILITY_NOT_FOUND);
                    }
                    if (resolved->schema_hash != requirement.expected_schema_hash)
                    {
                        releaseMount(mount_slot, EMountState::INACTIVE, false);
                        return lux::cxx::unexpected(EScriptSystemError::SCRIPT_CAPABILITY_SCHEMA_MISMATCH);
                    }
                    mount.capabilities.push_back(*resolved);
                }
            }
            catch (const std::bad_alloc&)
            {
                releaseMount(mount_slot, EMountState::INACTIVE, false);
                return lux::cxx::unexpected(EScriptSystemError::ALLOCATION_FAILURE);
            }

            auto instance = createInstanceRecord(mount_slot);
            if (!instance)
            {
                const auto error = instance.error();
                releaseMount(mount_slot, EMountState::INACTIVE, false);
                return lux::cxx::unexpected(error);
            }
            mount.instance = *instance;

            const ScriptInstanceCreateContext create_context{mount.authored->asset,
                                                             mount.scope,
                                                             std::addressof(mount.behavior),
                                                             mount.instance,
                                                             mount.capabilities};
            const auto created = mount.backend->createInstance(mount.backend->context,
                                                               create_context,
                                                               *mount.artifact.artifact,
                                                               mount.backend_instance);
            if (created != EScriptBackendResult::SUCCESS)
            {
                const auto error = backendError(created);
                releaseMount(mount_slot, EMountState::INACTIVE, false);
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
                    releaseMount(mount_slot, EMountState::INACTIVE, false);
                    return lux::cxx::unexpected(EScriptSystemError::SYMBOL_NOT_FOUND);
                }

                const auto prepared_method = mount.backend->prepareMethod(mount.backend->context,
                                                                          mount.backend_instance,
                                                                          *function,
                                                                          method.call);
                if (prepared_method != EScriptBackendResult::SUCCESS || !method.call)
                {
                    const auto error = backendError(prepared_method);
                    releaseMount(mount_slot, EMountState::INACTIVE, false);
                    return lux::cxx::unexpected(error);
                }
                if (method.used_by_binding && mount.backend->prepareStepMethod != nullptr)
                {
                    const auto prepared_step = mount.backend->prepareStepMethod(mount.backend->context,
                                                                                mount.backend_instance,
                                                                                *function,
                                                                                method.step);
                    if (prepared_step != EScriptBackendResult::SUCCESS)
                    {
                        const auto error = backendError(prepared_step);
                        releaseMount(mount_slot, EMountState::INACTIVE, false);
                        return lux::cxx::unexpected(error);
                    }
                }
            }

            const auto binding_end = mount.binding_first + mount.binding_count;
            for (std::size_t binding_slot{mount.binding_first}; binding_slot < binding_end; ++binding_slot)
            {
                const auto& binding = bindings[binding_slot];
                const auto& authored = mount.authored->bindings[binding_slot - mount.binding_first];
                const auto* function = mount.artifact.artifact->findExport(methods[binding.method_slot].symbol);
                const bool is_hook = binding.kind == EBindingKind::HOOK;
                const bool is_signature_valid =
                    is_hook ? sameHookSignature(*function, hooks[binding.bucket_slot].endpoint->signature)
                            : sameEventSignature(*function, events[binding.bucket_slot].endpoint->payload_type);
                if (!is_signature_valid)
                {
                    releaseMount(mount_slot, EMountState::INACTIVE, false);
                    return lux::cxx::unexpected(EScriptSystemError::SIGNATURE_MISMATCH);
                }

                if (!is_hook)
                {
                    const auto& endpoint = *events[binding.bucket_slot].endpoint;
                    const bool is_targeted = endpoint.route == EEventRoute::ENTITY_TARGETED;
                    const bool has_entity_scope = std::holds_alternative<EntityScriptScope>(mount.scope);
                    if (is_targeted && !has_entity_scope)
                    {
                        releaseMount(mount_slot, EMountState::INACTIVE, false);
                        return lux::cxx::unexpected(EScriptSystemError::SCOPE_MISMATCH);
                    }
                }
                static_cast<void>(authored);
            }

            mount.state = EMountState::INITIALIZED;
            return {};
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptSystemError> publishMount(std::uint32_t mount_slot) noexcept
        {
            auto& mount = mounts[mount_slot];
            if (mount.state != EMountState::INITIALIZED || !mount.gameplay_lifetime_started)
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
            mount.state = EMountState::ACTIVE;
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
            if (suppress_attachment_signal || !source.all_of<detail::ScriptAttachment>(entity))
                return;

            const auto mount_slot = source.get<detail::ScriptAttachment>(entity).mount_slot;
            if (mount_slot >= mounts.size())
                return;
            auto& mount = mounts[mount_slot];
            if (mount.entity != entity)
                return;

            if (destroying && mount.state == EMountState::ACTIVE)
            {
                mount.state = EMountState::RETIRING;
                mount.pending_end_reason = EScriptEndPlayReason::ENTITY_DESTROYED;
                deactivate(mount);
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
                if (bucket.handler_capacity == 0U)
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
            if (mount.state != EMountState::ACTIVE || mount.instance != resume.instance)
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
                &State::discardAwaitableErased
            };
            const auto result = continuation->backend.resume(continuation->backend.state, context, packet);

            continuation = continuations.find(continuationKey(resume.continuation));
            instance = instances.find(instanceKey(resume.instance));
            if (continuation == nullptr || instance == nullptr || instance->mount_slot >= mounts.size())
                return {};
            auto& current_mount = mounts[instance->mount_slot];
            if (current_mount.instance != resume.instance || current_mount.state != EMountState::ACTIVE)
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
                    return {};
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
                    EMountState::INACTIVE,
                    true,
                    EScriptEndPlayReason::RUNTIME_STOPPED
                );
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

    lux::cxx::expected<ScriptSystem, EScriptSystemError> ScriptSystem::create(
        const SimulationDescription& simulation,
        const ScriptSystemDescription& description,
        ecs::Registry& registry,
        const SimulationClock& clock,
        ScriptRuntimeLimits limits,
        ScriptArtifactResolver artifacts,
        WorldObjectResolver world,
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
                                    limits.simulation_delay_capacity == 0U;
        if (invalid_limits || artifacts.resolve == nullptr)
            return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);

        const auto enabled_mounts = std::count_if(description.mounts().begin(),
                                                  description.mounts().end(),
                                                  [](const auto& mount) noexcept { return mount.enabled; });
        if (enabled_mounts > limits.instance_capacity)
            return lux::cxx::unexpected(EScriptSystemError::CAPACITY_EXCEEDED);

        std::array<ScriptBackendDescriptor, kBackendKindCount> backend_table{};
        for (const auto& backend : backends)
        {
            const auto index = backendIndex(backend.kind);
            const bool is_invalid_kind =
                backend.kind == lux::rdesc::Script::Kind::UNKNOWN || index >= backend_table.size();
            const bool is_invalid_functions = backend.createInstance == nullptr || backend.prepareMethod == nullptr ||
                                              backend.releaseMethod == nullptr || backend.destroyInstance == nullptr;
            const bool is_invalid_step_pair =
                (backend.prepareStepMethod == nullptr) != (backend.releaseStepMethod == nullptr);
            if (is_invalid_kind || is_invalid_functions || is_invalid_step_pair)
                return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);
            if (backend_table[index].kind != lux::rdesc::Script::Kind::UNKNOWN)
                return lux::cxx::unexpected(EScriptSystemError::DUPLICATE_BACKEND_KIND);
            backend_table[index] = backend;
        }

        try
        {
            auto state = std::make_unique<State>();
            state->simulation = std::addressof(simulation);
            state->description = std::addressof(description);
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
                    !described || described.parameterCount() != hooks[index].signature.parameters.size() ||
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
                const bool is_invalid_identity = !events[index].system.valid() || !events[index].event.valid();
                const bool is_invalid_functions =
                    events[index].connect == nullptr || events[index].disconnect == nullptr;
                const bool is_invalid_signature =
                    !described || described.route() != events[index].route ||
                    described.payloadType() != events[index].payload_type.type_id ||
                    described.payloadSchemaName() != events[index].payload_type.canonical_name ||
                    events[index].payload_type.pass != lux::semantic::EValuePass::CONST_REF;
                if (is_invalid_identity || is_invalid_functions || is_invalid_signature)
                    return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);

                const auto inserted =
                    event_endpoint_index.emplace(EndpointKey{events[index].system.value, events[index].event.value},
                                                 static_cast<std::uint32_t>(index));
                if (!inserted.second)
                    return lux::cxx::unexpected(EScriptSystemError::DUPLICATE_ENDPOINT);
            }

            state->artifacts = artifacts;
            state->world = world;
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
            state->ingress = std::make_shared<State::AwaitableIngress>();
            state->ingress->awaitable_capacity = limits.awaitable_capacity;
            state->ingress->max_payload_bytes = limits.max_resume_payload_bytes;
            state->ingress->awaitables.reserve(limits.awaitable_capacity);
            state->ingress->resumes.prepare(limits.resume_queue_capacity);
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
        if (state_->stopping)
            return lux::cxx::unexpected(EScriptSystemError::ENDPOINT_BUSY);
        if (state_->prepare_state == EPrepareState::PREPARED)
            return {};
        if (state_->prepare_state == EPrepareState::ROLLBACK_PENDING)
            return lux::cxx::unexpected(EScriptSystemError::ENDPOINT_BUSY);
        if (state_->prepare_state == EPrepareState::PREPARING)
            return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);

        state_->prepare_state = EPrepareState::PREPARING;

        const auto layout = state_->buildLayout();
        if (!layout)
        {
            const auto rolled_back = state_->rollbackPrepare();
            return rolled_back ? layout : rolled_back;
        }

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
                if (initialized.error() == EScriptSystemError::WORLD_OBJECT_NOT_RESOLVED)
                {
                    state_->queueDirty(static_cast<std::uint32_t>(mount_slot));
                    continue;
                }
                const auto rolled_back = state_->rollbackPrepare();
                return rolled_back ? initialized : rolled_back;
            }
            if (state_->mounts[mount_slot].state == EMountState::INITIALIZED)
                state_->lifecycle_initialized.push_back(static_cast<std::uint32_t>(mount_slot));
        }

        for (const auto mount_slot : state_->lifecycle_initialized)
        {
            const auto begun = state_->beginPlayMount(mount_slot);
            if (!begun)
            {
                state_->releaseMount(mount_slot, EMountState::FAULTED, true);
                const auto rolled_back = state_->rollbackPrepare();
                return rolled_back ? begun : rolled_back;
            }
        }

        for (const auto mount_slot : state_->lifecycle_initialized)
        {
            if (state_->mounts[mount_slot].state != EMountState::INITIALIZED)
                continue;
            const auto published = state_->publishMount(mount_slot);
            if (!published)
            {
                state_->releaseMount(
                    mount_slot,
                    EMountState::FAULTED,
                    true,
                    EScriptEndPlayReason::FAULTED
                );
                const auto rolled_back = state_->rollbackPrepare();
                return rolled_back ? published : rolled_back;
            }
        }
        for (const auto mount_slot : state_->lifecycle_initialized)
        {
            if (state_->mounts[mount_slot].state == EMountState::INITIALIZED)
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

    lux::cxx::expected<void, EScriptSystemError> ScriptSystem::executeStablePoint() noexcept
    {
        if (!state_ || state_->prepare_state == EPrepareState::SHUT_DOWN)
            return lux::cxx::unexpected(EScriptSystemError::SHUT_DOWN);
        if (state_->stopping)
            return lux::cxx::unexpected(EScriptSystemError::ENDPOINT_BUSY);
        if (state_->prepare_state == EPrepareState::ROLLBACK_PENDING)
            return lux::cxx::unexpected(EScriptSystemError::ENDPOINT_BUSY);
        if (state_->prepare_state != EPrepareState::PREPARED)
            return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);

        state_->lifecycle_retirements.clear();
        state_->lifecycle_candidates.clear();
        for (const auto mount_slot : state_->retirement_queue)
        {
            auto& mount = state_->mounts[mount_slot];
            if (mount.state == EMountState::FAULTED)
            {
                state_->lifecycle_retirements.push_back(state_->beginRetirement(
                    mount_slot,
                    EScriptEndPlayReason::FAULTED,
                    EMountState::FAULTED,
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
            if (mount.state == EMountState::FAULTED ||
                (mount.state == EMountState::RETIRING && mount.retirement_queued))
                continue;

            const bool attachment_matches = state_->ownsAttachment(mount_slot, mount.entity);
            if (mount.state == EMountState::ACTIVE && attachment_matches)
                continue;

            if (mount.state != EMountState::INACTIVE)
            {
                state_->lifecycle_retirements.push_back(state_->beginRetirement(
                    mount_slot,
                    mount.pending_end_reason,
                    EMountState::INACTIVE,
                    attachment_matches
                ));
            }
            state_->lifecycle_candidates.push_back(mount_slot);
        }
        state_->retirement_queue.clear();
        state_->dirty_processing.clear();

        for (const auto& retirement : state_->lifecycle_retirements)
            state_->finishRetirement(retirement);

        state_->lifecycle_initialized.clear();
        for (const auto mount_slot : state_->lifecycle_candidates)
        {
            const auto initialized = state_->initializeMount(mount_slot);
            if (initialized)
            {
                if (state_->mounts[mount_slot].state == EMountState::INITIALIZED)
                    state_->lifecycle_initialized.push_back(mount_slot);
                continue;
            }

            const auto error = initialized.error();
            if (!first_error)
                first_error = error;
            if (error == EScriptSystemError::WORLD_OBJECT_NOT_RESOLVED)
            {
                state_->queueDirty(mount_slot);
                continue;
            }

            auto& failed_mount = state_->mounts[mount_slot];
            failed_mount.state = EMountState::FAULTED;
            state_->recordFailure(error, failed_mount);
        }

        for (const auto mount_slot : state_->lifecycle_initialized)
        {
            const auto begun = state_->beginPlayMount(mount_slot);
            if (!begun)
            {
                state_->releaseMount(mount_slot, EMountState::FAULTED, true);
                if (!first_error)
                    first_error = begun.error();
            }
        }
        for (const auto mount_slot : state_->lifecycle_initialized)
        {
            auto& mount = state_->mounts[mount_slot];
            if (mount.state != EMountState::INITIALIZED || !mount.gameplay_lifetime_started)
                continue;
            const auto published = state_->publishMount(mount_slot);
            if (!published)
            {
                const auto error = published.error();
                state_->releaseMount(
                    mount_slot,
                    EMountState::FAULTED,
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
            if (state_->mounts[mount_slot].state == EMountState::INITIALIZED &&
                state_->mounts[mount_slot].gameplay_lifetime_started)
            {
                state_->activateMount(mount_slot);
            }
        }

        if (!state_->promoteNextStepWaits() && !first_error)
            first_error = EScriptSystemError::INVOCATION_FAILURE;
        if (!state_->promoteSimulationDelays() && !first_error)
            first_error = EScriptSystemError::INVOCATION_FAILURE;

        const auto resumed = state_->drainResumes();
        if (!resumed && !first_error)
            first_error = resumed.error();

        return first_error ? lux::cxx::expected<void, EScriptSystemError>(lux::cxx::unexpected(*first_error))
                           : lux::cxx::expected<void, EScriptSystemError>{};
    }

    lux::cxx::expected<void, EScriptSystemError> ScriptSystem::shutdown() noexcept
    {
        if (!state_ || state_->prepare_state == EPrepareState::SHUT_DOWN)
            return {};

        state_->stopping = true;
        {
            std::lock_guard lock{state_->ingress->mutex};
            state_->ingress->stopping = true;
        }
        {
            std::lock_guard lock{state_->delay_mutex};
            state_->next_step_waits.clear();
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
            if (mount.state == EMountState::INACTIVE && !mount.backend_instance)
                continue;
            const auto reason = mount.state == EMountState::FAULTED
                ? EScriptEndPlayReason::FAULTED
                : EScriptEndPlayReason::RUNTIME_STOPPED;
            state_->lifecycle_retirements.push_back(state_->beginRetirement(
                static_cast<std::uint32_t>(index),
                reason,
                EMountState::INACTIVE,
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
        state_->instances.clear();
        state_->published_capabilities.clear();
        {
            std::lock_guard lock{state_->ingress->mutex};
            state_->ingress->awaitables.clear();
            state_->ingress->resumes.clear();
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
        std::lock_guard lock{state_->ingress->mutex};
        return state_->ingress->awaitables.size();
    }

    ScriptRuntimeStats ScriptSystem::stats() const noexcept
    {
        ScriptRuntimeStats result;
        if (!state_)
            return result;
        result.active_instances = state_->active_mount_count;
        result.active_continuations = state_->continuations.size();
        if (state_->ingress)
        {
            std::lock_guard lock{state_->ingress->mutex};
            result.active_awaitables = state_->ingress->awaitables.size();
            result.resume_queue_depth = state_->ingress->resumes.count;
            result.resume_queue_high_water = state_->ingress->resumes.high_water;
        }
        {
            std::lock_guard lock{state_->delay_mutex};
            result.next_step_waits = state_->next_step_waits.size();
            result.simulation_delay_waits = state_->simulation_delays.size();
        }
        return result;
    }

    std::span<const ScriptSystemFailure> ScriptSystem::failures() const noexcept
    {
        return state_ ? std::span<const ScriptSystemFailure>(state_->failures) : std::span<const ScriptSystemFailure>{};
    }
}
