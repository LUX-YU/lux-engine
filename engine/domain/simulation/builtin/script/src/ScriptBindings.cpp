#include <lux/engine/simulation/script/ScriptBindings.hpp>
#include <lux/engine/simulation/scripting/ScriptSignatureCompatibility.hpp>

#include <algorithm>
#include <exception>
#include <limits>
#include <new>
#include <utility>

namespace lux::simulation::script::detail
{
    std::size_t ScriptBindings::EndpointKeyHash::operator()(EndpointKey key) const noexcept
    {
        const auto first = std::hash<std::uint64_t>{}(key.system);
        const auto second = std::hash<std::uint64_t>{}(key.endpoint);
        return first ^ (second + 0x9e3779b9U + (first << 6U) + (first >> 2U));
    }

    ScriptBindings::BatchTicket::BatchTicket(BatchTicket&& other) noexcept
        : owner_(std::exchange(other.owner_, nullptr)) {}

    ScriptBindings::BatchTicket::~BatchTicket() noexcept
    {
        if (owner_ != nullptr)
            owner_->discardReservation();
    }

    void ScriptBindings::discardReservation() noexcept
    {
        staged_inputs_ = {};
        staged_placements_ = {};
        reservation_active_ = false;
    }

    std::optional<std::uint32_t> ScriptBindings::findHook(HookScriptTarget target) const noexcept
    {
        const auto found = hook_index_.find({target.system.value, target.hook.value});
        return found == hook_index_.end() ? std::nullopt : std::optional{found->second};
    }

    std::optional<std::uint32_t> ScriptBindings::findEvent(EventScriptTarget target) const noexcept
    {
        const auto found = event_index_.find({target.system.value, target.event.value});
        return found == event_index_.end() ? std::nullopt : std::optional{found->second};
    }

    const ScriptEventEndpointDescriptor& ScriptBindings::eventEndpoint(std::uint32_t slot) const noexcept
    {
        return event_endpoints_[slot];
    }

    ScriptBindings::Result ScriptBindings::prepare(
        const SimulationDescription& simulation,
        const ScriptRuntimeCapacityPlan& capacity,
        std::span<const ScriptHookEndpointDescriptor> hooks,
        std::span<const ScriptEventEndpointDescriptor> events,
        ScriptBindingDispatch dispatch,
        std::size_t max_resume_payload
    ) noexcept
    {
        try
        {
            dispatch_ = dispatch;
            max_resume_payload_ = max_resume_payload;
            binding_capacity_ = capacity.binding_capacity;
            method_capacity_ = capacity.method_capacity;
            hook_endpoints_.assign(hooks.begin(), hooks.end());
            event_endpoints_.assign(events.begin(), events.end());
            hook_index_.reserve(hooks.size());
            event_index_.reserve(events.size());
            const auto validated = validateEndpoints(simulation);
            if (!validated)
                return validated;
            hooks_.resize(hooks.size());
            events_.resize(events.size());
            configurations_.resize(capacity.mount_capacity);
            bindings_.reserve(binding_capacity_);
            descriptions_.reserve(binding_capacity_);
            symbols_.reserve(method_capacity_);
            pending_unlinks_.reserve(capacity.mount_capacity);
            hook_counts_.resize(hooks.size());
            event_counts_.resize(events.size());
            hook_reservations_.resize(hooks.size());
            event_reservations_.resize(events.size());
            for (std::size_t index{}; index < hooks_.size(); ++index)
            {
                hooks_[index].owner = this;
                hooks_[index].slot = static_cast<std::uint32_t>(index);
            }
            for (std::size_t index{}; index < events_.size(); ++index)
            {
                events_[index].owner = this;
                events_[index].slot = static_cast<std::uint32_t>(index);
            }
            for (const auto& planned : capacity.endpoint_capacities)
            {
                if (const auto* target = std::get_if<HookScriptTarget>(&planned.target))
                {
                    const auto endpoint = findHook(*target);
                    if (!endpoint)
                        return lux::cxx::unexpected(EScriptSystemError::SCRIPT_ENDPOINT_NOT_FOUND);
                    if (hooks_[*endpoint].capacity != 0U || planned.handler_capacity == 0U)
                        return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);
                    hooks_[*endpoint].capacity = planned.handler_capacity;
                }
                else
                {
                    const auto endpoint = findEvent(std::get<EventScriptTarget>(planned.target));
                    if (!endpoint)
                        return lux::cxx::unexpected(EScriptSystemError::SCRIPT_ENDPOINT_NOT_FOUND);
                    if (events_[*endpoint].capacity != 0U || planned.handler_capacity == 0U)
                        return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);
                    events_[*endpoint].capacity = planned.handler_capacity;
                }
            }
            for (auto& bucket : hooks_)
                bucket.handlers.reserve(bucket.capacity);
            for (auto& bucket : events_)
                if (bucket.handlers.prepare(bucket.capacity) == EEndpointMutationError::ALLOCATION_FAILURE)
                    return lux::cxx::unexpected(EScriptSystemError::ALLOCATION_FAILURE);
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(EScriptSystemError::ALLOCATION_FAILURE);
        }
    }

    lux::cxx::expected<ScriptBindings::BatchTicket, EScriptSystemError> ScriptBindings::reserveBatch(
        std::span<const ScriptRuntimeMount> inputs,
        std::span<const ScriptMountPlacement> placements
    ) noexcept
    {
        if (reservation_active_ || traversal_depth_ != 0U)
            return lux::cxx::unexpected(EScriptSystemError::ENDPOINT_BUSY);
        if (inputs.size() != placements.size())
            return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);
        std::copy(hook_counts_.begin(), hook_counts_.end(), hook_reservations_.begin());
        std::copy(event_counts_.begin(), event_counts_.end(), event_reservations_.begin());
        assembly_endpoint_count_visits_ += hook_counts_.size() + event_counts_.size();
        auto binding_count = bindings_.size();
        auto method_count = symbols_.size();
        for (std::size_t item{}; item < inputs.size(); ++item)
        {
            const auto& input = inputs[item];
            const auto placement = placements[item];
            if (placement.slot >= configurations_.size())
                return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);
            if (placement.existing)
            {
                const auto& config = configurations_[placement.slot];
                if (config.count != input.bindings.size() || !std::equal(input.bindings.begin(),
                        input.bindings.end(), descriptions_.begin() + config.first))
                    return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);
                continue;
            }
            if (input.bindings.size() > binding_capacity_ - binding_count)
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
                    const auto endpoint = findHook(*target);
                    if (!endpoint)
                        return lux::cxx::unexpected(EScriptSystemError::SCRIPT_ENDPOINT_NOT_FOUND);
                    if (++hook_reservations_[*endpoint] > hooks_[*endpoint].capacity)
                        return lux::cxx::unexpected(EScriptSystemError::CAPACITY_EXCEEDED);
                }
                else
                {
                    const auto endpoint = findEvent(std::get<EventScriptTarget>(binding.target));
                    if (!endpoint)
                        return lux::cxx::unexpected(EScriptSystemError::SCRIPT_ENDPOINT_NOT_FOUND);
                    const bool invalid_scope = std::holds_alternative<SimulationScriptScope>(input.scope) &&
                        event_endpoints_[*endpoint].route == EEventRoute::ENTITY_TARGETED;
                    if (invalid_scope)
                        return lux::cxx::unexpected(EScriptSystemError::SCOPE_MISMATCH);
                    if (++event_reservations_[*endpoint] > events_[*endpoint].capacity)
                        return lux::cxx::unexpected(EScriptSystemError::CAPACITY_EXCEEDED);
                }
            }
            if (unique_methods > method_capacity_ - method_count)
                return lux::cxx::unexpected(EScriptSystemError::CAPACITY_EXCEEDED);
            method_count += unique_methods;
        }
        staged_inputs_ = inputs;
        staged_placements_ = placements;
        reservation_active_ = true;
        return BatchTicket{*this};
    }

    void ScriptBindings::commitBatch(BatchTicket&& ticket) noexcept
    {
        if (ticket.owner_ != this || !reservation_active_)
            std::terminate();
        std::copy(hook_reservations_.begin(), hook_reservations_.end(), hook_counts_.begin());
        std::copy(event_reservations_.begin(), event_reservations_.end(), event_counts_.begin());
        assembly_endpoint_count_visits_ += hook_counts_.size() + event_counts_.size();
        for (std::size_t index{}; index < staged_inputs_.size(); ++index)
        {
            const auto placement = staged_placements_[index];
            if (placement.existing)
                continue;
            const auto& input = staged_inputs_[index];
            auto& config = configurations_[placement.slot];
            config.first = bindings_.size();
            config.count = input.bindings.size();
            config.methods.method_first = symbols_.size();
            config.entity_scope = std::holds_alternative<EntityScriptScope>(input.scope);
            for (const auto& binding : input.bindings)
            {
                auto symbol = std::find(symbols_.begin() + config.methods.method_first, symbols_.end(), binding.symbol);
                Binding runtime;
                runtime.method = static_cast<std::uint32_t>(symbol - symbols_.begin());
                if (symbol == symbols_.end())
                    symbols_.push_back(binding.symbol);
                if (const auto* target = std::get_if<HookScriptTarget>(&binding.target))
                {
                    runtime.kind = EBindingKind::HOOK;
                    runtime.bucket = *findHook(*target);
                }
                else
                {
                    runtime.kind = EBindingKind::EVENT;
                    runtime.bucket = *findEvent(std::get<EventScriptTarget>(binding.target));
                }
                bindings_.push_back(runtime);
                descriptions_.push_back(binding);
            }
            symbols_.push_back({});
            symbols_.push_back({});
            config.methods.method_count = symbols_.size() - config.methods.method_first;
        }
        ticket.owner_ = nullptr;
        discardReservation();
    }

    ScriptBindingLayout ScriptBindings::layout(std::uint32_t slot) const noexcept
    {
        return configurations_[slot].methods;
    }

    bool ScriptBindings::matches(
        std::uint32_t slot, std::span<const ScriptBindingDescription> bindings
    ) const noexcept
    {
        const auto& config = configurations_[slot];
        return config.count == bindings.size() &&
            std::equal(bindings.begin(), bindings.end(), descriptions_.begin() + config.first);
    }

    lux::script::ScriptSymbolId ScriptBindings::methodSymbol(std::size_t method_slot) const noexcept
    {
        return symbols_[method_slot];
    }

    bool ScriptBindings::methodUsedByBinding(std::size_t method_slot) const noexcept
    {
        return symbols_[method_slot] != lux::script::InvalidScriptSymbolId;
    }

    std::size_t ScriptBindings::backingBytes() const noexcept
    {
        return bindings_.capacity() * sizeof(Binding) + descriptions_.capacity() * sizeof(ScriptBindingDescription) +
            configurations_.capacity() * sizeof(Configuration) +
            symbols_.capacity() * sizeof(lux::script::ScriptSymbolId);
    }

    ScriptBindings::Result ScriptBindings::validateMethods(
        std::uint32_t slot, const lux::script::ScriptArtifact& artifact
    ) const noexcept
    {
        const auto& config = configurations_[slot];
        for (std::size_t index{config.first}; index < config.first + config.count; ++index)
        {
            const auto& binding = bindings_[index];
            const auto* function = artifact.findExport(symbols_[binding.method]);
            if (function == nullptr)
                return lux::cxx::unexpected(EScriptSystemError::SYMBOL_NOT_FOUND);
            const bool is_hook = binding.kind == EBindingKind::HOOK;
            const bool signature_matches = is_hook
                ? sameScriptHookSignature(*function, hook_endpoints_[binding.bucket].signature)
                : sameScriptEventSignature(*function, event_endpoints_[binding.bucket].payload_type);
            if (!signature_matches)
                return lux::cxx::unexpected(EScriptSystemError::SIGNATURE_MISMATCH);
            const bool invalid_scope = !is_hook && !config.entity_scope &&
                event_endpoints_[binding.bucket].route == EEventRoute::ENTITY_TARGETED;
            if (invalid_scope)
                return lux::cxx::unexpected(EScriptSystemError::SCOPE_MISMATCH);
        }
        return {};
    }

    ScriptBindings::Result ScriptBindings::publish(
        std::uint32_t slot, ScriptInstanceId instance, ecs::Entity entity
    ) noexcept
    {
        auto& config = configurations_[slot];
        if (config.published || config.pending_unlink || traversal_depth_ != 0U)
            return lux::cxx::unexpected(EScriptSystemError::ENDPOINT_BUSY);
        for (std::size_t index{config.first}; index < config.first + config.count; ++index)
        {
            auto& binding = bindings_[index];
            const ScriptMethodReference handler{slot, binding.method, instance};
            EScriptSystemError error{};
            bool failed{};
            if (binding.kind == EBindingKind::HOOK)
            {
                auto& bucket = hooks_[binding.bucket];
                if (bucket.handlers.size() >= bucket.capacity)
                {
                    failed = true;
                    error = EScriptSystemError::CAPACITY_EXCEEDED;
                }
                else if (const auto inserted = bucket.handlers.tryEmplace(handler))
                    binding.registration = {inserted->index, inserted->gen};
                else
                {
                    failed = true;
                    error = EScriptSystemError::ALLOCATION_FAILURE;
                }
            }
            else
            {
                auto& bucket = events_[binding.bucket];
                const bool broadcast = event_endpoints_[binding.bucket].route == EEventRoute::SIMULATION_BROADCAST;
                if (!broadcast && entity == ecs::NullEntity)
                {
                    unlink(slot);
                    return lux::cxx::unexpected(EScriptSystemError::SCOPE_MISMATCH);
                }
                const auto inserted = bucket.handlers.connect(entity, handler, broadcast);
                if (inserted)
                    binding.registration = inserted.token;
                else
                {
                    failed = true;
                    error = inserted.error == EEndpointMutationError::CAPACITY_EXCEEDED
                        ? EScriptSystemError::CAPACITY_EXCEEDED : EScriptSystemError::ALLOCATION_FAILURE;
                }
            }
            if (failed)
            {
                unlink(slot);
                return lux::cxx::unexpected(error);
            }
        }
        config.published = true;
        return {};
    }

    void ScriptBindings::unlink(std::uint32_t slot) noexcept
    {
        auto& config = configurations_[slot];
        for (std::size_t index{config.first}; index < config.first + config.count; ++index)
        {
            auto& binding = bindings_[index];
            if (!binding.registration.valid())
                continue;
            if (binding.kind == EBindingKind::HOOK)
                hooks_[binding.bucket].handlers.erase(HandlerKey{binding.registration.slot,
                    binding.registration.generation});
            else
                static_cast<void>(events_[binding.bucket].handlers.disconnect(binding.registration));
            binding.registration = {};
        }
        config.pending_unlink = false;
        config.published = false;
    }

    void ScriptBindings::withdraw(std::uint32_t slot) noexcept
    {
        auto& config = configurations_[slot];
        config.published = false;
        if (traversal_depth_ == 0U)
            unlink(slot);
        else if (!config.pending_unlink)
        {
            config.pending_unlink = true;
            pending_unlinks_.push_back(slot);
        }
    }

    ScriptBindings::Traversal::~Traversal() noexcept { owner_.finishTraversal(); }

    void ScriptBindings::finishTraversal() noexcept
    {
        if (--traversal_depth_ != 0U)
            return;
        for (const auto slot : pending_unlinks_)
            unlink(slot);
        pending_unlinks_.clear();
    }

    void ScriptBindings::visitHook(std::uint32_t slot, lux_script_call_frame& frame) noexcept
    {
        Traversal traversal{*this};
        for (const auto& handler : hooks_[slot].handlers.values())
            if (configurations_[handler.mount_slot].published)
                dispatch_.invoke(dispatch_.context, handler, frame, true);
    }

    void ScriptBindings::visitEvent(std::uint32_t slot, ecs::Entity entity, lux_script_call_frame& frame) noexcept
    {
        Traversal traversal{*this};
        const auto invoke = [this, &frame](const ScriptMethodReference& handler) noexcept {
            if (configurations_[handler.mount_slot].published)
                dispatch_.invoke(dispatch_.context, handler, frame, false);
        };
        if (event_endpoints_[slot].route == EEventRoute::SIMULATION_BROADCAST)
            events_[slot].handlers.forEachAll(invoke);
        else
            events_[slot].handlers.forEachTarget(entity, invoke);
    }

    void ScriptBindings::hookEntry(void* context, lux_script_call_frame& frame) noexcept
    {
        const auto& bucket = *static_cast<HookBucket*>(context);
        auto& dispatch = bucket.owner->dispatch_;
        dispatch.hook(dispatch.context, bucket.slot, frame);
    }

    void ScriptBindings::eventEntry(void* context, ecs::Entity entity, lux_script_call_frame& frame) noexcept
    {
        const auto& bucket = *static_cast<EventBucket*>(context);
        auto& dispatch = bucket.owner->dispatch_;
        dispatch.event(dispatch.context, bucket.slot, entity, frame);
    }

    ScriptBindings::Result ScriptBindings::connect() noexcept
    {
        for (auto& bucket : hooks_)
        {
            if (bucket.capacity == 0U || bucket.token.valid())
                continue;
            const auto& endpoint = hook_endpoints_[bucket.slot];
            const auto result = endpoint.connect(endpoint.context, &bucket, &hookEntry);
            if (!result)
                return lux::cxx::unexpected(EScriptSystemError::ENDPOINT_CONNECTION_FAILURE);
            bucket.token = result.token;
        }
        for (auto& bucket : events_)
        {
            const auto& endpoint = event_endpoints_[bucket.slot];
            const auto& projection = endpoint.payload_projection;
            const bool supports_wait =
                projection.copy != nullptr && projection.owned_layout.size <= max_resume_payload_;
            if ((bucket.capacity == 0U && !supports_wait) || bucket.token.valid())
                continue;
            const auto result = endpoint.connect(endpoint.context, &bucket, &eventEntry);
            if (!result)
                return lux::cxx::unexpected(EScriptSystemError::ENDPOINT_CONNECTION_FAILURE);
            bucket.token = result.token;
        }
        return {};
    }

    ScriptBindings::Result ScriptBindings::disconnect() noexcept
    {
        bool busy{};
        const auto disconnect_bucket = [&busy](auto& bucket, const auto& endpoint) noexcept -> Result {
            if (!bucket.token.valid())
                return {};
            const auto error = endpoint.disconnect(endpoint.context, bucket.token);
            if (error == EEndpointMutationError::DISPATCH_ACTIVE || error == EEndpointMutationError::WRITER_ACTIVE)
            {
                busy = true;
                return {};
            }
            if (error != EEndpointMutationError::NONE && error != EEndpointMutationError::INVALID_TOKEN)
                return lux::cxx::unexpected(EScriptSystemError::ENDPOINT_CONNECTION_FAILURE);
            bucket.token = {};
            return {};
        };
        for (auto& bucket : hooks_)
            if (const auto result = disconnect_bucket(bucket, hook_endpoints_[bucket.slot]); !result)
                return result;
        for (auto& bucket : events_)
            if (const auto result = disconnect_bucket(bucket, event_endpoints_[bucket.slot]); !result)
                return result;
        return busy ? Result{lux::cxx::unexpected(EScriptSystemError::ENDPOINT_BUSY)} : Result{};
    }

    ScriptBindings::Result ScriptBindings::validateEndpoints(const SimulationDescription& simulation)
    {
        for (std::size_t index{}; index < hook_endpoints_.size(); ++index)
        {
            if (index >= std::numeric_limits<std::uint32_t>::max())
                return lux::cxx::unexpected(EScriptSystemError::CAPACITY_EXCEEDED);

            const auto& endpoint = hook_endpoints_[index];
            const auto described = simulation.findHookPoint(endpoint.system, endpoint.hook);
            const bool is_invalid_identity = !endpoint.system.valid() || !endpoint.hook.valid();
            const bool is_invalid_functions = endpoint.connect == nullptr || endpoint.disconnect == nullptr;
            const bool is_invalid_signature =
                !described || !described.scriptCapable() ||
                described.parameterCount() != endpoint.signature.parameters.size() ||
                !endpoint.signature.returns.empty();
            if (is_invalid_identity || is_invalid_functions || is_invalid_signature)
                return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);

            for (std::size_t parameter{}; parameter < described.parameterCount(); ++parameter)
            {
                if (described.parameterAt(parameter) != endpoint.signature.parameters[parameter])
                    return lux::cxx::unexpected(EScriptSystemError::SIGNATURE_MISMATCH);
            }
            const auto inserted =
                hook_index_.emplace(EndpointKey{endpoint.system.value, endpoint.hook.value},
                                            static_cast<std::uint32_t>(index));
            if (!inserted.second)
                return lux::cxx::unexpected(EScriptSystemError::DUPLICATE_ENDPOINT);
        }

        for (std::size_t index{}; index < event_endpoints_.size(); ++index)
        {
            if (index >= std::numeric_limits<std::uint32_t>::max())
                return lux::cxx::unexpected(EScriptSystemError::CAPACITY_EXCEEDED);

            const auto& endpoint = event_endpoints_[index];
            const auto described = simulation.findEvent(endpoint.system, endpoint.event);
            const auto& owned = endpoint.payload_projection.owned_layout;
            const auto* builtin = lux::semantic::builtinLayout(owned.type_id);
            const bool is_invalid_builtin = builtin != nullptr &&
                (builtin->canonical_name != owned.canonical_name || builtin->abi_kind != owned.abi_kind ||
                 builtin->size != owned.size || builtin->alignment != owned.alignment);
            const bool is_invalid_identity = !endpoint.system.valid() || !endpoint.event.valid();
            const bool is_invalid_functions =
                endpoint.connect == nullptr || endpoint.disconnect == nullptr;
            const bool is_invalid_signature =
                !described || !described.dispatchHook().scriptCapable() ||
                described.route() != endpoint.route ||
                described.payloadType() != endpoint.payload_type.type_id ||
                described.payloadSchemaName() != endpoint.payload_type.canonical_name ||
                endpoint.payload_type.pass != lux::semantic::EValuePass::CONST_REF ||
                owned.type_id != endpoint.payload_type.type_id ||
                owned.canonical_name != endpoint.payload_type.canonical_name || owned.abi_kind == 0U ||
                owned.size == 0U || owned.alignment == 0U ||
                (owned.alignment & (owned.alignment - 1U)) != 0U || is_invalid_builtin;
            if (is_invalid_identity || is_invalid_functions || is_invalid_signature)
                return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);

            const auto inserted =
                event_index_.emplace(EndpointKey{endpoint.system.value, endpoint.event.value},
                                             static_cast<std::uint32_t>(index));
            if (!inserted.second)
                return lux::cxx::unexpected(EScriptSystemError::DUPLICATE_ENDPOINT);
        }

        return {};
    }

}
