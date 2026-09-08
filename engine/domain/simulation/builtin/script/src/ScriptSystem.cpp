#include <lux/engine/simulation/ScriptSystem.hpp>
#include <lux/engine/simulation/script/ScriptExecution.hpp>
#include <lux/engine/simulation/script/ScriptPreparer.hpp>
#include <lux/engine/simulation/abilities/DelayAbility.hpp>
#include "DelayAbility.ability.generated.hpp"
#include <entt/signal/sigh.hpp>
#include <algorithm>
#include <limits>
#include <optional>
#include <utility>
#include <vector>
#if defined(LUX_SCRIPT_OWNER_AFFINITY_PROBE)
#include <thread>
#endif

namespace lux::simulation::script
{
    namespace
    {
        enum class EPrepareState : std::uint8_t
        {
            CREATED, PREPARING, ROLLBACK_PENDING, PREPARED, SHUT_DOWN,
        };
    }

    struct ScriptSystem::State final
    {
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

        using Handler = detail::ScriptMethodReference;
        using RetirementRecord = detail::ScriptInstances::Retirement;
        const SimulationDescription* simulation{};
        ecs::Registry* registry{};
        const SimulationClock* clock{};
        ScriptRuntimeLimits limits;
        detail::ScriptPreparer preparer;
        detail::ScriptInstances instance_owner;
        detail::ScriptBindings binding_owner;
        detail::ScriptEventWaits event_owner;
        detail::ScriptTimers timer_owner;
        detail::ScriptCompletionIngress ingress;
        detail::ScriptExecution execution_owner{instance_owner, binding_owner, event_owner, timer_owner, ingress};
        std::vector<ScriptSystemFailure> failures;
        std::vector<std::uint32_t> retirement_queue;
        SparseMountQueue dirty_current;
        SparseMountQueue dirty_processing;
        std::vector<std::uint32_t> lifecycle_candidates;
        std::vector<std::uint32_t> lifecycle_initialized;
        std::vector<RetirementRecord> lifecycle_retirements;
        std::uint64_t last_stable_step{};
        std::uint64_t event_occurrences{};
        std::uint64_t invocation_failures{};
        std::size_t endpoint_dispatch_depth{};
        entt::connection constructed;
        entt::connection updated;
        entt::connection destroyed;
        bool stopping{};
        bool stop_requested{};
        bool region_active{};
        EPrepareState prepare_state{EPrepareState::CREATED};
#if defined(LUX_SCRIPT_OWNER_AFFINITY_PROBE)
        std::thread::id execution_owner_thread;
        std::size_t execution_depth{};
#endif
        [[nodiscard]] bool enterExecutionOwner() noexcept
        {
#if defined(LUX_SCRIPT_OWNER_AFFINITY_PROBE)
            const auto current = std::this_thread::get_id();
            if (execution_owner_thread == std::thread::id{})
                execution_owner_thread = current;
            if (execution_owner_thread != current)
                return false;
            ++execution_depth;
#endif
            return true;
        }

        void leaveExecutionOwner() noexcept
        {
#if defined(LUX_SCRIPT_OWNER_AFFINITY_PROBE)
            if (execution_owner_thread != std::this_thread::get_id() || execution_depth == 0U)
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
        void faultInvocation(std::uint32_t slot, lux::script::ScriptSymbolId symbol,
            EScriptSystemError error, std::int32_t status = 0) noexcept
        {
            ++invocation_failures;
            execution_owner.invalidateAdmission(instance_owner.fault(slot));
            binding_owner.withdraw(slot);
            queueRetirement(slot);
            recordFailure(error, slot, symbol, status);
        }
        struct FailurePort final { State* owner{}; } failure_port{this};
        static void faultErased(void* context, std::uint32_t slot, lux::script::ScriptSymbolId symbol,
            EScriptSystemError error, std::int32_t status) noexcept
        {
            static_cast<FailurePort*>(context)->owner->faultInvocation(slot, symbol, error, status);
        }
        struct BindingPort final { State* owner{}; } binding_port{this};
        static void invokeHookLane(void* context, std::uint32_t bucket, lux_script_call_frame& frame) noexcept
        {
            auto& owner = *static_cast<BindingPort*>(context)->owner;
            ExecutionOwnerScope execution{owner};
            if (!execution)
                return;
            if (!owner.region_active && owner.instance_owner.protectedCount() == 0U)
                std::terminate(); // Native caller violated the explicit execution-region contract.
            detail::ScriptInstances::Protection region{owner.instance_owner};
            ++owner.endpoint_dispatch_depth;
            owner.binding_owner.visitHook(bucket,
                [&](const Handler& handler) noexcept { owner.execution_owner.invoke(handler, frame, true); });
            --owner.endpoint_dispatch_depth;
        }
        static void dispatchEvent(void* context, std::uint32_t bucket, ecs::Entity entity,
            lux_script_call_frame& frame) noexcept
        {
            auto& owner = *static_cast<BindingPort*>(context)->owner;
            ExecutionOwnerScope execution{owner};
            if (!execution)
                return;
            if (!owner.region_active && owner.instance_owner.protectedCount() == 0U)
                std::terminate();
            detail::ScriptInstances::Protection region{owner.instance_owner};
            ++owner.event_occurrences;
            ++owner.endpoint_dispatch_depth;
            {
                const auto& endpoint = owner.binding_owner.eventEndpoint(bucket);
                const auto target = endpoint.route == EEventRoute::SIMULATION_BROADCAST ? ecs::NullEntity : entity;
                auto claimed = owner.event_owner.claim(bucket, target);
                owner.binding_owner.visitEvent(bucket, entity,
                    [&](const Handler& handler) noexcept { owner.execution_owner.invoke(handler, frame, false); });
                for (std::size_t index{}; index < claimed.size(); ++index)
                    if (const auto waiter = claimed.at(index))
                        owner.execution_owner.completeClaimedEventWaiter(*waiter, frame);
            }
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
            execution_owner.invalidateInstance(instance_owner.view(slot).retiring_instance);
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
            {
                execution_owner.beginInstance(mount.instance, slot);
                event_owner.beginInstance(mount.instance);
                timer_owner.beginInstance(mount.instance);
            }
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
                execution_owner.invalidateAdmission(mount.retiring_instance);
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
        [[nodiscard]] lux::cxx::expected<ScriptStablePointReport, EScriptSystemError> drainResumes() noexcept
        {
            ScriptStablePointReport report;
            bool completion_failed{};
            auto batch = execution_owner.resumeBatch();
            while (const auto resumed = batch.next())
            {
                if (!*resumed && !report.first_instance_error)
                    report.first_instance_error = resumed->error();
                // Do not recapture: completion admission after EVERY pop uses the same bounded frontier.
                if (!execution_owner.drainExternalCompletions())
                    completion_failed = true;
            }
            if (completion_failed)
                return lux::cxx::unexpected(EScriptSystemError::INVOCATION_FAILURE);
            return report;
        }
    };

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

            const auto delay_binding = lux::script::bindScriptAbility<DelayAbility>(state->timer_owner);
            const auto delay_publication = publishScriptAbility(delay_binding);
            const auto catalog = state->preparer.prepareCatalog(artifacts, backends, capabilities, delay_publication);
            if (!catalog)
                return lux::cxx::unexpected(catalog.error());
            const auto instance_layout = state->instance_owner.prepare(
                capacity, limits.instance_capacity, registry, host
            );
            if (!instance_layout)
                return lux::cxx::unexpected(instance_layout.error());
            state->execution_owner.prepare(limits, state->instance_owner.identityCapacity(), capacity.method_capacity,
                {&state->failure_port, &State::faultErased});
            state->event_owner.prepare(
                limits.event_wait_capacity, state->instance_owner.identityCapacity(), events.size()
            );
            state->timer_owner.prepare(clock, limits, real_delay, state->execution_owner,
                state->instance_owner.identityCapacity());
            state->ingress.prepare(
                limits.external_completion_capacity, state->execution_owner.physicalAwaitableCapacity()
            );
            const auto binding_layout = state->binding_owner.prepare(simulation, capacity, hooks, events,
                {&state->binding_port, &State::invokeHookLane, &State::dispatchEvent},
                limits.max_resume_payload_bytes);
            if (!binding_layout)
                return lux::cxx::unexpected(binding_layout.error());
            state->failures.reserve(limits.failure_capacity);
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

    ScriptSystem::ScriptSystem(ScriptSystem&& other) noexcept
    {
        if (other.state_ && other.state_->region_active)
            std::terminate();
        state_ = std::move(other.state_);
    }
    ScriptSystem& ScriptSystem::operator=(ScriptSystem&& other) noexcept
    {
        if (this == &other)
            return *this;
        if (other.state_ && other.state_->region_active)
            std::terminate();
        // Replacement has the same owner-boundary and noexcept failure policy as destruction.
        // Keep both States owned until the old destination has completed its shutdown protocol.
        if (state_ && state_->prepare_state != EPrepareState::SHUT_DOWN && !shutdown())
            std::terminate();
        state_ = std::move(other.state_);
        return *this;
    }

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
        state_->execution_owner.enablePrepared();
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
        if (state_->region_active || state_->endpoint_dispatch_depth != 0U ||
            state_->instance_owner.protectedCount() != 0U ||
            state_->execution_owner.resultPins() != 0U || state_->event_owner.claimedCount() != 0U)
            return lux::cxx::unexpected(EScriptSystemError::ENDPOINT_BUSY);
        if (state_->stop_requested)
            return shutdown();
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

    lux::cxx::expected<ScriptStablePointReport, EScriptSystemError> ScriptSystem::executeStablePoint() noexcept
    {
        if (!state_ || state_->prepare_state == EPrepareState::SHUT_DOWN)
            return lux::cxx::unexpected(EScriptSystemError::SHUT_DOWN);
        State::ExecutionOwnerScope execution{*state_};
        if (!execution || state_->endpoint_dispatch_depth != 0U || state_->instance_owner.protectedCount() != 0U)
            return lux::cxx::unexpected(EScriptSystemError::ENDPOINT_BUSY);
        if (!state_->region_active)
            return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);
        const auto step = state_->clock->snapshot().step_index;
        // Step zero is the standalone, not-yet-executing test/preparation boundary.
        if (step != 0U && step == state_->last_stable_step)
            return {};
        state_->last_stable_step = step;
        state_->ingress.beginDrain();
        std::optional<EScriptSystemError> first_error;
        if (!state_->timer_owner.promoteNextStep() && !first_error)
            first_error = EScriptSystemError::INVOCATION_FAILURE;
        if (!state_->timer_owner.promoteSimulationDelay() && !first_error)
            first_error = EScriptSystemError::INVOCATION_FAILURE;
        if (!state_->execution_owner.drainExternalCompletions() && !first_error)
            first_error = EScriptSystemError::INVOCATION_FAILURE;

        auto resumed = state_->drainResumes();
        if (!resumed && !first_error)
            first_error = resumed.error();
        if (first_error)
            return lux::cxx::unexpected(*first_error);
        return resumed;
    }

    lux::cxx::expected<void, EScriptSystemError>
    ScriptSystem::mountResolvedBatch(std::span<const ScriptRuntimeMount> mounts) noexcept
    {
        if (!state_ || state_->stopping || state_->prepare_state == EPrepareState::SHUT_DOWN)
            return lux::cxx::unexpected(EScriptSystemError::SHUT_DOWN);
        State::ExecutionOwnerScope execution{*state_};
        const bool busy = !execution || state_->region_active || state_->prepare_state != EPrepareState::PREPARED ||
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
        state_->ingress.capture();
    }

    ScriptSystem::ExecutionRegion::ExecutionRegion(ExecutionRegion&& other) noexcept
        : owner_(std::exchange(other.owner_, nullptr)) {}

    ScriptSystem::ExecutionRegion::~ExecutionRegion() noexcept
    {
        if (owner_ != nullptr && !finish())
            std::terminate();
    }

    lux::cxx::expected<void, EScriptSystemError> ScriptSystem::ExecutionRegion::finish() noexcept
    {
        if (owner_ == nullptr)
            return {};
        auto& state = *owner_->state_;
        if (state.endpoint_dispatch_depth != 0U || state.instance_owner.protectedCount() != 0U ||
            state.execution_owner.resultPins() != 0U || state.event_owner.claimedCount() != 0U)
            return lux::cxx::unexpected(EScriptSystemError::ENDPOINT_BUSY);
        state.region_active = false;
        owner_ = nullptr;
        return {};
    }

    lux::cxx::expected<ScriptSystem::ExecutionRegion, EScriptSystemError>
    ScriptSystem::beginExecutionRegion() noexcept
    {
        if (!state_ || state_->prepare_state == EPrepareState::SHUT_DOWN || state_->stopping)
            return lux::cxx::unexpected(EScriptSystemError::SHUT_DOWN);
        if (state_->region_active || state_->prepare_state != EPrepareState::PREPARED ||
            state_->endpoint_dispatch_depth != 0U || state_->instance_owner.protectedCount() != 0U)
            return lux::cxx::unexpected(EScriptSystemError::ENDPOINT_BUSY);
        state_->region_active = true;
        return ExecutionRegion{*this};
    }

    lux::cxx::expected<void, EScriptSystemError> ScriptSystem::requestStop() noexcept
    {
        if (!state_ || state_->prepare_state == EPrepareState::SHUT_DOWN)
            return {};
        state_->stop_requested = true;
        return {};
    }

    lux::cxx::expected<void, EScriptSystemError> ScriptSystem::shutdown() noexcept
    {
        if (!state_ || state_->prepare_state == EPrepareState::SHUT_DOWN)
            return {};
        State::ExecutionOwnerScope execution{*state_};
        if (!execution)
            return lux::cxx::unexpected(EScriptSystemError::ENDPOINT_BUSY);

        if (state_->region_active || state_->instance_owner.protectedCount() != 0U ||
            state_->execution_owner.resultPins() != 0U)
            return lux::cxx::unexpected(EScriptSystemError::ENDPOINT_BUSY);

        state_->stopping = true;
        state_->instance_owner.stopInvocations();
        state_->execution_owner.stop();
        state_->timer_owner.stop();
        state_->ingress.stop();
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
        state_->execution_owner.shutdown();
        state_->event_owner.shutdown();
        state_->timer_owner.shutdown();
        state_->preparer.releaseCatalog();
        state_->instance_owner.finishShutdown();
        state_->prepare_state = EPrepareState::SHUT_DOWN;
        return {};
    }

    std::size_t ScriptSystem::activeInstanceCount() const noexcept
    {
        return state_ ? state_->instance_owner.activeCount() : 0U;
    }

    bool ScriptSystem::isShutdown() const noexcept
    {
        return !state_ || state_->prepare_state == EPrepareState::SHUT_DOWN;
    }

    bool ScriptSystem::inExecutionRegion() const noexcept
    {
        return state_ && state_->region_active;
    }

    std::size_t ScriptSystem::activeContinuationCount() const noexcept
    {
        return state_ ? state_->execution_owner.activeContinuations() : 0U;
    }

    std::size_t ScriptSystem::activeAwaitableCount() const noexcept
    {
        return state_ ? state_->execution_owner.activeAwaitables() : 0U;
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
        result.event_occurrences = state_->event_occurrences;
        result.invocation_failures = state_->invocation_failures;
        state_->execution_owner.writeStats(result);
        state_->event_owner.writeStats(result);
        state_->timer_owner.writeStats(result);
        state_->ingress.writeStats(result);
        return result;
    }

    std::span<const ScriptSystemFailure> ScriptSystem::failures() const noexcept
    {
        return state_ ? std::span<const ScriptSystemFailure>(state_->failures) : std::span<const ScriptSystemFailure>{};
    }
}
