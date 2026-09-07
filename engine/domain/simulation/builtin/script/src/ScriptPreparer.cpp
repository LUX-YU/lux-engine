#include <lux/engine/simulation/script/ScriptPreparer.hpp>
#include <lux/engine/simulation/scripting/ScriptLifecycle.hpp>

#include <algorithm>
#include <new>

namespace lux::simulation::script::detail
{
    namespace
    {
        [[nodiscard]] EScriptSystemError backendError(EScriptBackendResult result) noexcept
        {
            if (result == EScriptBackendResult::CAPACITY_EXCEEDED)
                return EScriptSystemError::CAPACITY_EXCEEDED;
            if (result == EScriptBackendResult::ALLOCATION_FAILURE)
                return EScriptSystemError::ALLOCATION_FAILURE;
            return EScriptSystemError::BACKEND_FAILURE;
        }
    }

    ScriptPreparer::Result ScriptPreparer::prepareCatalog(
        ScriptArtifactResolver artifacts,
        std::span<const ScriptBackendDescriptor> backends,
        std::span<const ScriptApiCapabilityPublication> capabilities,
        const ScriptApiCapabilityPublication& delay
    ) noexcept
    {
        artifacts_ = artifacts;
        for (const auto& backend : backends)
        {
            const auto index = static_cast<std::size_t>(backend.kind);
            const bool invalid_kind = backend.kind == lux::rdesc::Script::Kind::UNKNOWN || index >= backends_.size();
            const bool invalid_functions = backend.createInstance == nullptr || backend.prepareMethod == nullptr ||
                backend.releaseMethod == nullptr || backend.destroyInstance == nullptr;
            if (invalid_kind || invalid_functions)
                return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);
            if (backends_[index].kind != lux::rdesc::Script::Kind::UNKNOWN)
                return lux::cxx::unexpected(EScriptSystemError::DUPLICATE_BACKEND_KIND);
            backends_[index] = backend;
        }
        try
        {
            capabilities_.reserve(capabilities.size() + 1U);
            const auto add = [this](const ScriptApiCapabilityPublication& capability) -> Result {
                if (!capability.contract.isValid() || capability.schema_hash == 0U || capability.dispatch == nullptr)
                    return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);
                capabilities_.push_back({lux::script::ScriptApiContractId{capability.contract.name()},
                    capability.schema_hash, capability.context, capability.dispatch, capability.schema_version,
                    capability.methods});
                return {};
            };
            for (const auto& capability : capabilities)
                if (const auto result = add(capability); !result)
                    return result;
            if (const auto result = add(delay); !result)
                return result;
            std::sort(capabilities_.begin(), capabilities_.end(), [](const auto& left, const auto& right) noexcept {
                return left.contract.hash() < right.contract.hash() ||
                    (left.contract.hash() == right.contract.hash() && left.contract.name() < right.contract.name());
            });
            for (std::size_t index{1U}; index < capabilities_.size(); ++index)
            {
                const auto& previous = capabilities_[index - 1U].contract;
                const auto& current = capabilities_[index].contract;
                if (previous.hash() == current.hash())
                    return lux::cxx::unexpected(previous.name() == current.name()
                        ? EScriptSystemError::SCRIPT_CAPABILITY_AMBIGUOUS_PROVIDER
                        : EScriptSystemError::SCRIPT_CAPABILITY_ID_COLLISION);
            }
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(EScriptSystemError::ALLOCATION_FAILURE);
        }
    }
    const ScriptBackendDescriptor* ScriptPreparer::backend(lux::rdesc::Script::Kind kind) const noexcept
    {
        const auto index = static_cast<std::size_t>(kind);
        if (index >= backends_.size() || backends_[index].kind != kind)
            return nullptr;
        return &backends_[index];
    }
    const PreparedScriptApiCapability* ScriptPreparer::capability(
        const lux::script::ScriptApiContractId& contract
    ) const noexcept
    {
        const auto found = std::find_if(capabilities_.begin(), capabilities_.end(),
            [&contract](const auto& value) noexcept { return value.contract == contract; });
        return found == capabilities_.end() ? nullptr : &*found;
    }
    void ScriptPreparer::releaseCatalog() noexcept { capabilities_.clear(); }

    ScriptPreparer::Result ScriptPreparer::prepareMount(
        ScriptInstances& instances,
        std::uint32_t slot,
        const ScriptBindings& bindings,
        const SimulationDescription& simulation,
        const ScriptRuntimeLimits& limits
    ) noexcept
    {
        auto started = instances.beginConstruction(slot);
        if (!started)
            return lux::cxx::unexpected(started.error());
        if (!*started)
            return {};
        auto construction = std::move(**started);
        ResolvedScriptArtifact resolved;
        bool resident{};
        {
            ScriptInstances::Protection protection{instances};
            resident = artifacts_.resolve(artifacts_.context, construction.assetId(), resolved);
        }
        if (!resident)
            return lux::cxx::unexpected(EScriptSystemError::ASSET_NOT_RESIDENT);
        construction.adoptArtifact(resolved);
        const auto* artifact = construction.artifact();
        if (artifact == nullptr)
            return lux::cxx::unexpected(EScriptSystemError::INVALID_ASSET);
        const auto* selected = backend(artifact->description().kind());
        if (selected == nullptr)
            return lux::cxx::unexpected(EScriptSystemError::BACKEND_NOT_AVAILABLE);
        construction.selectBackend(*selected);
        const auto lifecycle = artifact->description().lifecycle;
        if (const auto result = construction.selectLifecycle(lifecycle.begin_play, lifecycle.end_play); !result)
            return result;
        if (lifecycle.begin_play != lux::script::InvalidScriptSymbolId)
        {
            const auto* function = artifact->findExport(lifecycle.begin_play);
            if (function == nullptr || !validBeginPlay(*function))
                return lux::cxx::unexpected(EScriptSystemError::SIGNATURE_MISMATCH);
        }
        if (lifecycle.end_play != lux::script::InvalidScriptSymbolId)
        {
            const auto* function = artifact->findExport(lifecycle.end_play);
            if (function == nullptr || !validEndPlay(*function))
                return lux::cxx::unexpected(EScriptSystemError::SIGNATURE_MISMATCH);
        }
        const auto& requirements = artifact->description().api_requirements;
        if (const auto result = construction.reserveCapabilities(requirements.size()); !result)
            return result;
        for (const auto& requirement : requirements)
        {
            const auto* publication = capability(requirement.contract);
            if (publication == nullptr)
                return lux::cxx::unexpected(EScriptSystemError::SCRIPT_CAPABILITY_NOT_FOUND);
            if (publication->schema_hash != requirement.expected_schema_hash)
                return lux::cxx::unexpected(EScriptSystemError::SCRIPT_CAPABILITY_SCHEMA_MISMATCH);
            for (const auto& method : publication->methods)
            {
                if (method.kind != lux::script::EScriptApiMethodKind::ASYNC_OPERATION)
                    continue;
                for (const auto& result : method.results)
                {
                    const bool unsupported = result.size > limits.max_resume_payload_bytes ||
                        !supportsExternalResumeLayout(result.size, result.alignment);
                    if (unsupported)
                        return lux::cxx::unexpected(EScriptSystemError::SIGNATURE_MISMATCH);
                }
            }
            if (const auto result = construction.addCapability(*publication); !result)
                return result;
        }
        const auto& events = artifact->description().event_requirements;
        if (const auto result = construction.reserveEvents(events.size()); !result)
            return result;
        if (const auto result = construction.nextEventLayout(); !result)
            return result;
        for (const auto& requirement : events)
        {
            if (requirement.payload.size > limits.max_resume_payload_bytes)
                return lux::cxx::unexpected(EScriptSystemError::CAPACITY_EXCEEDED);
            const auto endpoint_slot = bindings.findEvent({lux::system::SystemInstanceId{requirement.system_id},
                EventPointId{requirement.event_id}});
            if (!endpoint_slot)
                return lux::cxx::unexpected(EScriptSystemError::SCRIPT_EVENT_NOT_FOUND);
            const auto described = simulation.findEvent(lux::system::SystemInstanceId{requirement.system_id},
                EventPointId{requirement.event_id});
            if (!eventMatches(requirement, described, bindings.eventEndpoint(*endpoint_slot)))
                return lux::cxx::unexpected(EScriptSystemError::SCRIPT_EVENT_SCHEMA_MISMATCH);
            construction.addEvent({&requirement, {}, *endpoint_slot, {requirement.payload.type_id,
                requirement.payload.abi_kind, requirement.payload.size, requirement.payload.alignment}});
        }
        if (const auto result = construction.allocateIdentity(); !result)
            return result;
        const auto created = construction.createBackend();
        if (created != EScriptBackendResult::SUCCESS)
            return lux::cxx::unexpected(backendError(created));
        for (std::size_t local{}; local < construction.methodCount(); ++local)
        {
            const auto& method = construction.method(local);
            if (method.symbol == lux::script::InvalidScriptSymbolId)
                continue;
            const auto* function = artifact->findExport(method.symbol);
            if (function == nullptr)
                return lux::cxx::unexpected(EScriptSystemError::SYMBOL_NOT_FOUND);
            const auto result = construction.prepareMethod(local, *function);
            if (result != EScriptBackendResult::SUCCESS || !method.backend)
                return lux::cxx::unexpected(backendError(result));
            const bool invalid_lifecycle = construction.lifecycleMethod(local) &&
                (!method.backend.synchronous || static_cast<bool>(method.backend.resumable));
            if (invalid_lifecycle)
                return lux::cxx::unexpected(EScriptSystemError::SIGNATURE_MISMATCH);
        }
        if (const auto result = bindings.validateMethods(slot, *artifact); !result)
            return result;
        construction.commit();
        return {};
    }

    bool ScriptPreparer::validBeginPlay(const lux::rdesc::ScriptFunction& function) noexcept
    {
        return function.args.empty() && function.returns.empty();
    }

    bool ScriptPreparer::validEndPlay(const lux::rdesc::ScriptFunction& function) noexcept
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

    bool ScriptPreparer::eventMatches(
        const lux::script::ScriptEventSourceDescription& requirement,
        const SimulationEventView& described,
        const ScriptEventEndpointDescriptor& endpoint
    ) noexcept
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
}
