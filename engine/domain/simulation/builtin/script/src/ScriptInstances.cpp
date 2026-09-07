#include <lux/engine/simulation/script/ScriptInstances.hpp>
#include <lux/engine/simulation/scripting/ScriptRuntimeAccess.hpp>
#include <lux/engine/simulation/scripting/ScriptLifecycle.hpp>

#include <algorithm>
#include <exception>
#include <new>
#include <utility>

namespace lux::simulation::script::detail
{

    ScriptInstances::BatchTicket::BatchTicket(BatchTicket&& other) noexcept
        : owner_(std::exchange(other.owner_, nullptr)) {}
    ScriptInstances::BatchTicket::~BatchTicket() noexcept
    {
        if (owner_ != nullptr)
            owner_->discardReservation();
    }
    std::span<const ScriptMountPlacement> ScriptInstances::BatchTicket::placements() const noexcept
    {
        return owner_->batch_slots_;
    }
    void ScriptInstances::discardReservation() noexcept
    {
        reservation_active_ = false;
        staged_inputs_ = {};
    }

    ScriptInstances::Result ScriptInstances::prepare(
        const ScriptRuntimeCapacityPlan& capacity,
        std::size_t instance_capacity,
        ecs::Registry& registry,
        ScriptHostApi host
    ) noexcept
    {
        try
        {
            registry_ = &registry;
            host_ = host;
            enabled_capacity_ = capacity.enabled_mount_capacity;
            instance_capacity_ = instance_capacity;
            mounts_.resize(capacity.mount_capacity);
            methods_.reserve(capacity.method_capacity);
            identities_.reserve(instance_capacity);
            mount_index_.reserve(enabled_capacity_);
            entity_associations_.reserve(enabled_capacity_ * 2U);
            changes_.reserve(capacity.mount_capacity);
            changed_.resize(capacity.mount_capacity);
            batch_ids_.reserve(enabled_capacity_);
            batch_slots_.reserve(enabled_capacity_);
            reserved_mounts_.resize(capacity.mount_capacity);
            batch_entities_.reserve(enabled_capacity_);
            static lux::cxx::ScopeIdSource<ScriptEventAdmissionScopeTag> scopes;
            event_scope_ = scopes.acquire();
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(EScriptSystemError::ALLOCATION_FAILURE);
        }
    }

    std::optional<std::uint32_t> ScriptInstances::findMount(ScriptMountId id) const noexcept
    {
        const auto found = std::lower_bound(mount_index_.begin(), mount_index_.end(), id.value,
            [](const auto& entry, std::uint64_t value) noexcept { return entry.first < value; });
        if (found == mount_index_.end() || found->first != id.value)
            return std::nullopt;
        return found->second;
    }

    lux::cxx::expected<ScriptInstances::BatchTicket, EScriptSystemError> ScriptInstances::reserveBatch(
        std::span<const ScriptRuntimeMount> inputs, const ScriptBindings& bindings, bool initial
    ) noexcept
    {
        if (reservation_active_ || protection_count_ != 0U)
            return lux::cxx::unexpected(EScriptSystemError::ENDPOINT_BUSY);
        if (inputs.size() > enabled_capacity_)
            return lux::cxx::unexpected(EScriptSystemError::CAPACITY_EXCEEDED);
        batch_ids_.clear();
        batch_slots_.clear();
        batch_entities_.clear();
        bool has_new_configuration{};
        for (const auto& input : inputs)
        {
            batch_ids_.push_back(input.id.value);
            if (const auto* entity = std::get_if<EntityScriptScope>(&input.scope))
                batch_entities_.push_back(entity->self);
        }
        std::sort(batch_ids_.begin(), batch_ids_.end());
        std::sort(batch_entities_.begin(), batch_entities_.end());
        const bool duplicate_id = std::adjacent_find(batch_ids_.begin(), batch_ids_.end()) != batch_ids_.end();
        const bool duplicate_entity =
            std::adjacent_find(batch_entities_.begin(), batch_entities_.end()) != batch_entities_.end();
        if (duplicate_id || duplicate_entity)
            return lux::cxx::unexpected(EScriptSystemError::SCOPE_MISMATCH);
        auto count = configured_count_;
        std::uint32_t next_slot{};
        for (const auto& input : inputs)
        {
            const bool invalid_identity = !input.id.valid() || input.asset.isNull();
            if (invalid_identity || input.scope.valueless_by_exception())
                return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);
            const auto existing = findMount(input.id);
            ++assembly_configuration_slot_visits_;
            auto slot = input.configuration_index;
            if (existing)
            {
                if (slot != kInvalidPreparedMethod && slot != *existing)
                    return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);
                slot = *existing;
            }
            else
            {
                if (!has_new_configuration)
                {
                    std::fill(reserved_mounts_.begin(), reserved_mounts_.end(), 0U);
                    assembly_configuration_slot_visits_ += reserved_mounts_.size();
                    has_new_configuration = true;
                }
                if (slot == kInvalidPreparedMethod)
                {
                    if (!initial)
                        return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);
                    while (next_slot < mounts_.size() &&
                        (mounts_[next_slot].id.valid() || reserved_mounts_[next_slot] != 0U))
                        ++next_slot;
                    slot = next_slot;
                }
                if (slot >= mounts_.size() || mounts_[slot].id.valid() || reserved_mounts_[slot] != 0U)
                    return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);
                reserved_mounts_[slot] = 1U;
            }
            batch_slots_.push_back({slot, existing.has_value()});
            if (const auto* entity = std::get_if<EntityScriptScope>(&input.scope))
            {
                if (entity->self == ecs::NullEntity || !registry_->valid(entity->self))
                    return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);
                if (registry_->all_of<ScriptAttachment>(entity->self) || entity_associations_.contains(entity->self))
                    return lux::cxx::unexpected(EScriptSystemError::SCOPE_MISMATCH);
            }
            if (existing)
            {
                const auto& mount = mounts_[slot];
                const bool invalid_shape = mount.asset != input.asset ||
                    mount.entity_scope != std::holds_alternative<EntityScriptScope>(input.scope);
                if (invalid_shape || !bindings.matches(slot, input.bindings))
                    return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);
                if (mount.state == EScriptMountState::FAULTED ||
                    std::holds_alternative<SimulationScriptScope>(input.scope))
                    return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);
                const bool busy = mount.pending_scope || mount.unconsumed_result ||
                    (mount.state != EScriptMountState::INACTIVE && mount.state != EScriptMountState::RETIRING);
                if (busy)
                    return lux::cxx::unexpected(EScriptSystemError::ENDPOINT_BUSY);
            }
            else if (++count > enabled_capacity_)
                return lux::cxx::unexpected(EScriptSystemError::CAPACITY_EXCEEDED);
        }
        staged_inputs_ = inputs;
        reservation_active_ = true;
        return BatchTicket{*this};
    }

    void ScriptInstances::commitBatch(BatchTicket&& ticket, const ScriptBindings& bindings) noexcept
    {
        if (ticket.owner_ != this || !reservation_active_)
            std::terminate();
        for (std::size_t index{}; index < staged_inputs_.size(); ++index)
        {
            const auto& input = staged_inputs_[index];
            const auto placement = batch_slots_[index];
            auto& mount = mounts_[placement.slot];
            if (!placement.existing)
            {
                ++configured_count_;
                mount.id = input.id;
                mount.asset = input.asset;
                mount.entity_scope = std::holds_alternative<EntityScriptScope>(input.scope);
                mount.status.id = input.id;
                const auto layout = bindings.layout(placement.slot);
                mount.method_first = layout.method_first;
                mount.method_count = layout.method_count;
                while (methods_.size() < layout.method_first + layout.method_count)
                {
                    const auto method = methods_.size();
                    methods_.push_back({bindings.methodSymbol(method), {}, bindings.methodUsedByBinding(method)});
                }
                const auto position = std::lower_bound(mount_index_.begin(), mount_index_.end(), input.id.value,
                    [](const auto& entry, std::uint64_t value) noexcept { return entry.first < value; });
                mount_index_.insert(position, {input.id.value, placement.slot});
            }
            mount.admission_order = ++admission_sequence_;
            mount.pending_scope = input.scope;
            ++pending_count_;
            if (const auto* entity = std::get_if<EntityScriptScope>(&input.scope))
                entity_associations_.emplace(entity->self, placement.slot);
            ++mount.status.submission;
            mount.status.submission_state = EScriptMountSubmissionState::ACCEPTED;
            mount.status.submitted_scope = input.scope;
            markStatus(placement.slot);
        }
        ticket.owner_ = nullptr;
        discardReservation();
    }

    void ScriptInstances::markStatus(std::uint32_t slot) noexcept
    {
        auto& mount = mounts_[slot];
        if (!mount.id.valid())
            return;
        ++mount.status.revision;
        mount.status.state = mount.state;
        mount.status.instance = mount.instance;
        if (mount.state != EScriptMountState::INACTIVE)
            mount.status.scope = mount.scope;
        if (changed_[slot] != 0U)
            return;
        if (changes_.size() == changes_.capacity())
            std::terminate();
        changed_[slot] = 1U;
        changes_.push_back(slot);
    }

    ScriptMountView ScriptInstances::view(std::uint32_t slot) const noexcept
    {
        const auto& mount = mounts_[slot];
        return {mount.id, mount.asset, mount.scope, mount.instance, mount.retiring_instance, mount.entity, mount.state,
            mount.pending_end_reason, mount.admission_order, mount.pending_scope.has_value(), mount.retirement_queued,
            mount.gameplay_lifetime_started, mount.status.reclaimed};
    }

    std::optional<ScriptMountStatus> ScriptInstances::query(ScriptMountId id) const noexcept
    {
        const auto slot = findMount(id);
        return slot ? std::optional{mounts_[*slot].status} : std::nullopt;
    }
    ScriptMountStatusCollection ScriptInstances::collect(std::span<ScriptMountStatus> output) noexcept
    {
        const auto count = (std::min)(output.size(), changes_.size());
        for (std::size_t index{}; index < count; ++index)
        {
            const auto slot = changes_[index];
            output[index] = mounts_[slot].status;
            mounts_[slot].unconsumed_result = false;
            changed_[slot] = 0U;
        }
        changes_.erase(changes_.begin(), changes_.begin() + count);
        return {count, changes_.size()};
    }
    void ScriptInstances::writeStats(ScriptRuntimeStats& output) const noexcept
    {
        output.configured_mounts = configured_count_;
        output.assembly_configuration_slot_visits = assembly_configuration_slot_visits_;
        output.pending_mounts = pending_count_;
        output.active_instances = active_count_;
        output.mount_backing_bytes = mounts_.capacity() * sizeof(Mount);
        output.method_backing_bytes = methods_.capacity() * sizeof(ScriptPreparedMethod);
        output.mount_feedback_backing_bytes = changes_.capacity() * sizeof(std::uint32_t) + changed_.capacity();
    }

    ScriptInstances::Construction::Construction(Construction&& other) noexcept
        : owner_(std::exchange(other.owner_, nullptr)), slot_(other.slot_) {}
    ScriptInstances::Construction::~Construction() noexcept
    {
        if (owner_ != nullptr)
            owner_->rollbackConstruction(slot_);
    }
    lux::asset::AssetId ScriptInstances::Construction::assetId() const noexcept { return owner_->mounts_[slot_].asset; }
    const lux::script::ScriptArtifact* ScriptInstances::Construction::artifact() const noexcept
    {
        return owner_->mounts_[slot_].artifact.artifact;
    }
    void ScriptInstances::Construction::adoptArtifact(ResolvedScriptArtifact artifact) noexcept
    {
        owner_->mounts_[slot_].artifact = artifact;
    }
    void ScriptInstances::Construction::selectBackend(const ScriptBackendDescriptor& backend) noexcept
    {
        owner_->mounts_[slot_].backend = &backend;
    }
    lux::cxx::expected<std::uint32_t, EScriptSystemError> ScriptInstances::claimMethod(
        Mount& mount, lux::script::ScriptSymbolId symbol
    ) noexcept
    {
        if (symbol == lux::script::InvalidScriptSymbolId)
            return kInvalidPreparedMethod;
        const auto end = mount.method_first + mount.method_count;
        for (std::size_t slot{mount.method_first}; slot < end; ++slot)
            if (methods_[slot].symbol == symbol)
                return static_cast<std::uint32_t>(slot);
        for (std::size_t slot{mount.method_first}; slot < end; ++slot)
            if (methods_[slot].symbol == lux::script::InvalidScriptSymbolId)
            {
                methods_[slot].symbol = symbol;
                return static_cast<std::uint32_t>(slot);
            }
        return lux::cxx::unexpected(EScriptSystemError::CAPACITY_EXCEEDED);
    }
    ScriptInstances::Result ScriptInstances::Construction::selectLifecycle(
        lux::script::ScriptSymbolId begin, lux::script::ScriptSymbolId end
    ) noexcept
    {
        auto& mount = owner_->mounts_[slot_];
        const auto first = owner_->claimMethod(mount, begin);
        const auto last = owner_->claimMethod(mount, end);
        if (!first || !last)
            return lux::cxx::unexpected(!first ? first.error() : last.error());
        mount.begin_play_method = *first;
        mount.end_play_method = *last;
        return {};
    }
    ScriptInstances::Result ScriptInstances::Construction::reserveCapabilities(std::size_t count) noexcept
    {
        try
        {
            auto& capabilities = owner_->mounts_[slot_].capabilities;
            capabilities.clear();
            capabilities.reserve(count);
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(EScriptSystemError::ALLOCATION_FAILURE);
        }
    }
    ScriptInstances::Result
    ScriptInstances::Construction::addCapability(const PreparedScriptApiCapability& value) noexcept
    {
        try
        {
            owner_->mounts_[slot_].capabilities.push_back(value);
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(EScriptSystemError::ALLOCATION_FAILURE);
        }
    }
    ScriptInstances::Result ScriptInstances::Construction::nextEventLayout() noexcept
    {
        if (owner_->event_epoch_ == (std::numeric_limits<std::uint64_t>::max)())
            return lux::cxx::unexpected(EScriptSystemError::CAPACITY_EXCEEDED);
        owner_->mounts_[slot_].event_layout_epoch = ++owner_->event_epoch_;
        return {};
    }
    ScriptInstances::Result ScriptInstances::Construction::reserveEvents(std::size_t count) noexcept
    {
        try
        {
            auto& events = owner_->mounts_[slot_].event_sources;
            events.clear();
            events.reserve(count);
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(EScriptSystemError::ALLOCATION_FAILURE);
        }
    }
    void ScriptInstances::Construction::addEvent(PreparedScriptEventAdmission event) noexcept
    {
        owner_->mounts_[slot_].event_sources.push_back(event);
    }
    ScriptInstances::Result ScriptInstances::Construction::allocateIdentity() noexcept
    {
        if (owner_->identities_.size() >= owner_->instance_capacity_)
            return lux::cxx::unexpected(EScriptSystemError::CAPACITY_EXCEEDED);
        const auto inserted = owner_->identities_.tryEmplace(slot_);
        if (!inserted)
            return lux::cxx::unexpected(EScriptSystemError::ALLOCATION_FAILURE);
        auto& mount = owner_->mounts_[slot_];
        mount.instance = {inserted->index + 1U, inserted->gen};
        for (std::uint32_t local{}; local < mount.event_sources.size(); ++local)
            mount.event_sources[local].admission = ScriptRuntimeAccess::admission(
                owner_->event_scope_, mount.instance, mount.event_layout_epoch, local);
        return {};
    }
    EScriptBackendResult ScriptInstances::Construction::createBackend() noexcept
    {
        auto& mount = owner_->mounts_[slot_];
        const ScriptInstanceCreateContext context{mount.asset, mount.scope, &mount.behavior, mount.instance,
            mount.capabilities, mount.event_sources};
        Protection protection{*owner_};
        return mount.backend->createInstance(mount.backend->context, context, *mount.artifact.artifact,
            mount.backend_instance);
    }
    std::size_t ScriptInstances::Construction::methodCount() const noexcept
    {
        return owner_->mounts_[slot_].method_count;
    }
    const ScriptPreparedMethod& ScriptInstances::Construction::method(std::size_t local) const noexcept
    {
        return owner_->methods_[owner_->mounts_[slot_].method_first + local];
    }
    bool ScriptInstances::Construction::lifecycleMethod(std::size_t local) const noexcept
    {
        const auto& mount = owner_->mounts_[slot_];
        const auto method = mount.method_first + local;
        return method == mount.begin_play_method || method == mount.end_play_method;
    }
    EScriptBackendResult ScriptInstances::Construction::prepareMethod(
        std::size_t local, const lux::rdesc::ScriptFunction& function
    ) noexcept
    {
        auto& mount = owner_->mounts_[slot_];
        auto& method = owner_->methods_[mount.method_first + local];
        Protection protection{*owner_};
        return mount.backend->prepareMethod(mount.backend->context, mount.backend_instance, function, method.backend);
    }
    void ScriptInstances::Construction::commit() noexcept
    {
        owner_->mounts_[slot_].state = EScriptMountState::INITIALIZED;
        owner_->markStatus(slot_);
        owner_ = nullptr;
    }

    lux::cxx::expected<std::optional<ScriptInstances::Construction>, EScriptSystemError>
    ScriptInstances::beginConstruction(std::uint32_t slot) noexcept
    {
        auto& mount = mounts_[slot];
        const bool no_input = !mount.id.valid() || !mount.pending_scope || mount.state == EScriptMountState::FAULTED;
        const bool already_prepared = mount.state == EScriptMountState::ACTIVE ||
            mount.state == EScriptMountState::INITIALIZED;
        if (no_input || already_prepared)
            return std::optional<Construction>{};
        if (protection_count_ != 0U || !mount.status.reclaimed || mount.cleanup_claimed)
            return lux::cxx::unexpected(EScriptSystemError::ENDPOINT_BUSY);
        mount.scope = *mount.pending_scope;
        mount.pending_scope.reset();
        --pending_count_;
        if (const auto* entity = std::get_if<EntityScriptScope>(&mount.scope))
        {
            const bool invalid_entity = entity->self == ecs::NullEntity || !registry_->valid(entity->self);
            const bool occupied = !invalid_entity && registry_->all_of<ScriptAttachment>(entity->self);
            if (invalid_entity || occupied)
            {
                entity_associations_.erase(entity->self);
                mount.state = EScriptMountState::INACTIVE;
                mount.status.submission_state = EScriptMountSubmissionState::REJECTED;
                mount.status.submission_error = occupied ? EScriptSystemError::SCOPE_MISMATCH :
                    EScriptSystemError::INVALID_INPUT;
                mount.unconsumed_result = true;
                markStatus(slot);
                return std::optional<Construction>{};
            }
            mount.entity = entity->self;
        }
        else
            mount.entity = ecs::NullEntity;
        mount.state = EScriptMountState::CONSTRUCTING;
        mount.status.reclaimed = false;
        markStatus(slot);
        ScriptRuntimeAccess::attach(mount.behavior, mount.scope, host_);
        return std::optional{Construction{*this, slot}};
    }

    void ScriptInstances::deactivate(Mount& mount) noexcept
    {
        if (!mount.active_counted)
            return;
        mount.active_counted = false;
        --active_count_;
    }
    ScriptInstanceId ScriptInstances::revoke(std::uint32_t slot) noexcept
    {
        auto& mount = mounts_[slot];
        const auto instance = mount.instance;
        if (instance.valid())
        {
            static_cast<void>(identities_.erase(key(instance)));
            mount.retiring_instance = instance;
            mount.instance = {};
        }
        return instance;
    }
    ScriptInstanceId ScriptInstances::fault(std::uint32_t slot) noexcept
    {
        auto& mount = mounts_[slot];
        mount.state = EScriptMountState::FAULTED;
        markStatus(slot);
        mount.pending_end_reason = EScriptEndPlayReason::FAULTED;
        deactivate(mount);
        return revoke(slot);
    }
    void ScriptInstances::reject(std::uint32_t slot, EScriptSystemError error) noexcept
    {
        auto& mount = mounts_[slot];
        mount.state = EScriptMountState::FAULTED;
        mount.status.submission_state = EScriptMountSubmissionState::REJECTED;
        mount.status.submission_error = error;
        mount.unconsumed_result = true;
        markStatus(slot);
    }
    void ScriptInstances::recordError(std::uint32_t slot, EScriptSystemError error) noexcept
    {
        mounts_[slot].status.submission_error = error;
    }
    bool ScriptInstances::queueRetirement(std::uint32_t slot) noexcept
    {
        if (mounts_[slot].retirement_queued)
            return false;
        mounts_[slot].retirement_queued = true;
        return true;
    }
    ScriptInstances::Retirement ScriptInstances::claimRetirement(
        std::uint32_t slot, EScriptEndPlayReason reason, EScriptMountState final_state
    ) noexcept
    {
        auto& mount = mounts_[slot];
        if (mount.cleanup_claimed || protection_count_ != 0U)
            std::terminate(); // System must wait for its invoke/resume/copy/claim/cleanup safe boundary.
        mount.cleanup_claimed = true;
        mount.end_play_claimed = mount.gameplay_lifetime_started;
        mount.gameplay_lifetime_started = false;
        mount.status.retired_instance = mount.instance.valid() ? mount.instance : mount.retiring_instance;
        mount.state = EScriptMountState::RETIRING;
        markStatus(slot);
        deactivate(mount);
        static_cast<void>(revoke(slot));
        Retirement result;
        result.slot_ = slot;
        result.instance_ = mount.retiring_instance;
        result.epoch_ = ++mount.retirement_epoch;
        result.reason_ = reason;
        result.final_state_ = final_state;
        return result;
    }
    int ScriptInstances::invokeLifecycle(std::uint32_t method, const EScriptEndPlayReason* reason) noexcept
    {
        lux_script_call_frame frame{};
        lux_script_value_slot argument{};
        if (reason != nullptr)
        {
            argument = argumentSlot(*reason);
            frame.args = &argument;
            frame.arg_count = 1U;
        }
        const auto call = methods_[method].backend.synchronous;
        frame.user_context = call.context;
        Protection protection{*this};
        return call.invoke(&frame);
    }
    ScriptInstances::LifecycleResult ScriptInstances::beginPlay(std::uint32_t slot) noexcept
    {
        auto& mount = mounts_[slot];
        if (mount.state != EScriptMountState::INITIALIZED)
            return lux::cxx::unexpected(ScriptLifecycleCallError{EScriptSystemError::INVALID_INPUT});
        if (mount.begin_play_method != kInvalidPreparedMethod)
        {
            const auto method = mount.begin_play_method;
            const auto status = invokeLifecycle(method, nullptr);
            if (status != 0)
                return lux::cxx::unexpected(ScriptLifecycleCallError{
                    EScriptSystemError::INVOCATION_FAILURE, methods_[method].symbol, status});
        }
        mount.gameplay_lifetime_started = true;
        return {};
    }
    ScriptInstances::LifecycleResult ScriptInstances::endPlay(const Retirement& retirement) noexcept
    {
        auto& mount = mounts_[retirement.slot_];
        if (!mount.cleanup_claimed || mount.retirement_epoch != retirement.epoch_)
            std::terminate();
        if (!std::exchange(mount.end_play_claimed, false) || mount.end_play_method == kInvalidPreparedMethod)
            return {};
        const auto method = mount.end_play_method;
        const auto status = invokeLifecycle(method, &retirement.reason_);
        if (status != 0)
            return lux::cxx::unexpected(ScriptLifecycleCallError{
                EScriptSystemError::INVOCATION_FAILURE, methods_[method].symbol, status});
        return {};
    }
    void ScriptInstances::activate(std::uint32_t slot) noexcept
    {
        auto& mount = mounts_[slot];
        mount.state = EScriptMountState::ACTIVE;
        mount.active_counted = true;
        ++active_count_;
        mount.status.submission_state = EScriptMountSubmissionState::ACTIVATED;
        mount.unconsumed_result = true;
        markStatus(slot);
    }
    void ScriptInstances::resetMountRuntime(Mount& mount) noexcept
    {
        if (mount.entity != ecs::NullEntity)
            entity_associations_.erase(mount.entity);
        const auto end = mount.method_first + mount.method_count;
        for (std::size_t method{mount.method_first}; method < end; ++method)
            if (!methods_[method].used_by_binding)
                methods_[method] = {};
        mount.scope = SimulationScriptScope{};
        mount.behavior = {};
        mount.instance = {};
        mount.retiring_instance = {};
        mount.begin_play_method = kInvalidPreparedMethod;
        mount.end_play_method = kInvalidPreparedMethod;
        mount.capabilities.clear();
        mount.event_sources.clear();
        mount.artifact = {};
        mount.backend = nullptr;
        mount.backend_instance = {};
        mount.entity = ecs::NullEntity;
        mount.retirement_queued = false;
        mount.gameplay_lifetime_started = false;
        mount.cleanup_claimed = false;
        mount.end_play_claimed = false;
        mount.pending_end_reason = EScriptEndPlayReason::OBJECT_UNMATERIALIZED;
        mount.status.reclaimed = true;
    }
    void ScriptInstances::finishRetirement(const Retirement& retirement) noexcept
    {
        auto& mount = mounts_[retirement.slot_];
        if (!mount.cleanup_claimed || mount.retirement_epoch != retirement.epoch_ || mount.end_play_claimed)
            std::terminate();
        Protection protection{*this};
        if (mount.backend != nullptr && mount.backend_instance)
        {
            const auto* backend = mount.backend;
            const auto instance = mount.backend_instance;
            const auto end = mount.method_first + mount.method_count;
            for (std::size_t index{end}; index > mount.method_first; --index)
            {
                auto method = std::exchange(methods_[index - 1U].backend, {});
                if (method)
                    backend->releaseMethod(backend->context, instance, method);
            }
            mount.backend_instance = {};
            backend->destroyInstance(backend->context, instance);
        }
        mount.capabilities.clear();
        mount.event_sources.clear();
        const auto artifact = std::exchange(mount.artifact, {});
        if (artifact.lease != nullptr && artifact.release != nullptr)
            artifact.release(artifact.lease);
        resetMountRuntime(mount);
        mount.state = retirement.final_state_;
        if (mount.state == EScriptMountState::FAULTED)
        {
            mount.status.submission_state = EScriptMountSubmissionState::REJECTED;
            mount.unconsumed_result = true;
        }
        markStatus(retirement.slot_);
    }
    void ScriptInstances::rollbackConstruction(std::uint32_t slot) noexcept
    {
        auto retirement = claimRetirement(
            slot, EScriptEndPlayReason::OBJECT_UNMATERIALIZED, EScriptMountState::INACTIVE
        );
        static_cast<void>(endPlay(retirement));
        finishRetirement(retirement);
    }

    bool ScriptInstances::ownsAttachment(std::uint32_t slot, ecs::Entity entity) const noexcept
    {
        return entity != ecs::NullEntity && registry_->valid(entity) && registry_->all_of<ScriptAttachment>(entity) &&
            registry_->get<ScriptAttachment>(entity).mount_slot == slot;
    }
    ScriptInstances::Result ScriptInstances::projectAttachment(std::uint32_t slot) noexcept
    {
        const auto entity = mounts_[slot].entity;
        if (entity == ecs::NullEntity)
            return {};
        if (!registry_->valid(entity))
            return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);
        if (registry_->all_of<ScriptAttachment>(entity))
            return ownsAttachment(slot, entity)
                ? Result{} : Result{lux::cxx::unexpected(EScriptSystemError::SCOPE_MISMATCH)};
        suppress_attachment_signal_ = true;
        try
        {
            registry_->emplace<ScriptAttachment>(entity, slot);
        }
        catch (const std::bad_alloc&)
        {
            suppress_attachment_signal_ = false;
            return lux::cxx::unexpected(EScriptSystemError::ALLOCATION_FAILURE);
        }
        suppress_attachment_signal_ = false;
        return {};
    }
    void ScriptInstances::removeAttachment(std::uint32_t slot) noexcept
    {
        const auto entity = mounts_[slot].entity;
        if (!ownsAttachment(slot, entity))
            return;
        suppress_attachment_signal_ = true;
        registry_->remove<ScriptAttachment>(entity);
        suppress_attachment_signal_ = false;
    }
    std::optional<std::uint32_t> ScriptInstances::observeAttachment(ecs::Entity entity, bool destroying) noexcept
    {
        if (suppress_attachment_signal_ || !registry_->all_of<ScriptAttachment>(entity))
            return std::nullopt;
        const auto slot = registry_->get<ScriptAttachment>(entity).mount_slot;
        if (slot >= mounts_.size())
            return std::nullopt;
        auto& mount = mounts_[slot];
        if (mount.entity != entity)
            return std::nullopt;
        if (destroying && mount.state == EScriptMountState::ACTIVE)
        {
            mount.status.retired_instance = mount.instance.valid() ? mount.instance : mount.retiring_instance;
            mount.state = EScriptMountState::RETIRING;
            markStatus(slot);
            mount.pending_end_reason = EScriptEndPlayReason::ENTITY_DESTROYED;
            deactivate(mount);
            static_cast<void>(revoke(slot));
        }
        return slot;
    }
    void ScriptInstances::restorePendingAfterRollback() noexcept
    {
        for (std::uint32_t slot{}; slot < mounts_.size(); ++slot)
        {
            auto& mount = mounts_[slot];
            if (!mount.id.valid())
                continue;
            if (!mount.pending_scope)
                ++pending_count_;
            mount.pending_scope = mount.status.submitted_scope;
            if (const auto* entity = std::get_if<EntityScriptScope>(&*mount.pending_scope))
                entity_associations_.emplace(entity->self, slot);
        }
        active_count_ = 0U;
    }
    void ScriptInstances::finishShutdown() noexcept
    {
        identities_.clear();
        entity_associations_.clear();
        for (std::uint32_t slot{}; slot < mounts_.size(); ++slot)
        {
            auto& mount = mounts_[slot];
            if (!mount.pending_scope)
                continue;
            mount.pending_scope.reset();
            --pending_count_;
            mount.status.submission_state = EScriptMountSubmissionState::CANCELLED;
            mount.status.submission_error = EScriptSystemError::SHUT_DOWN;
            mount.unconsumed_result = true;
            markStatus(slot);
        }
    }
    lux::cxx::expected<ScriptEventSourceAccess, EScriptEventWaitError> ScriptInstances::eventSource(
        ScriptInstanceId instance, ScriptEventAdmissionHandle handle
    ) const noexcept
    {
        const auto* slot = identities_.find(key(instance));
        if (slot == nullptr || !active(instance))
            return lux::cxx::unexpected(EScriptEventWaitError::INVALID_INSTANCE);
        const auto& mount = mounts_[*slot];
        const auto local = ScriptRuntimeAccess::matchAdmission(handle, event_scope_, instance,
            mount.event_layout_epoch, mount.event_sources.size());
        if (!local)
            return lux::cxx::unexpected(EScriptEventWaitError::UNDECLARED_SOURCE);
        const auto& source = mount.event_sources[*local];
        return ScriptEventSourceAccess{source.endpoint_slot, source.payload, mount.scope};
    }
}
