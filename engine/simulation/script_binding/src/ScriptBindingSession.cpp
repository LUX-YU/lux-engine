#include <lux/engine/simulation/ScriptBindingSession.hpp>

#include <entt/entity/entity.hpp>
#include <entt/signal/sigh.hpp>

#include <algorithm>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace lux::simulation
{
    namespace
    {
        constexpr std::uint32_t kInvalidIndex =
            std::numeric_limits<std::uint32_t>::max();

        [[nodiscard]] std::size_t entityIndex(ecs::Entity entity) noexcept
        {
            return static_cast<std::size_t>(entt::to_entity(entity));
        }

        [[nodiscard]] bool sameType(
            const lux::rdesc::ScriptValueType& script_type,
            const lux::script::ScriptSemanticType& target_type
        ) noexcept
        {
            return script_type.type_id == target_type.type_id &&
                script_type.canonical_name == target_type.canonical_name &&
                script_type.pass == target_type.pass;
        }

        [[nodiscard]] bool sameHookSignature(
            const lux::rdesc::ScriptFunction& function,
            const SimulationHookPointView& hook
        ) noexcept
        {
            if (function.args.size() != hook.parameterCount() ||
                function.returns.size() != hook.returnCount())
            {
                return false;
            }
            for (std::size_t index{}; index < function.args.size(); ++index)
            {
                if (!sameType(function.args[index], hook.parameterAt(index)))
                    return false;
            }
            for (std::size_t index{}; index < function.returns.size(); ++index)
            {
                if (!sameType(function.returns[index], hook.returnAt(index)))
                    return false;
            }
            return true;
        }

        [[nodiscard]] bool sameEventSignature(
            const lux::rdesc::ScriptFunction& function,
            const SimulationEventView& event
        ) noexcept
        {
            if (!function.returns.empty())
                return false;
            if (event.payloadSchemaName().empty())
                return function.args.empty();
            return function.args.size() == 1U &&
                function.args.front().canonical_name ==
                    event.payloadSchemaName() &&
                function.args.front().type_id == event.payloadSchemaHash();
        }
    }

    struct ScriptBindingSession::State final
    {
        struct Hook final
        {
            std::size_t system{};
            std::size_t member{};
            ESystemHookCardinality cardinality{ESystemHookCardinality::MULTI};
            std::vector<std::uint32_t> handlers;
        };

        struct Event final
        {
            std::size_t system{};
            std::size_t member{};
            ScriptHookSlot dispatch_hook;
            ESystemEventTarget target{ESystemEventTarget::GLOBAL};
            std::vector<std::uint32_t> global_handlers;
        };

        struct Prepared final
        {
            lux::script::BoundScriptCall call;
            std::size_t backend{};
            lux::script::ScriptSymbolId symbol{};
            ecs::Entity entity{ecs::NullEntity};
            bool active{true};
            bool pending_disable{};
        };

        struct EntitySidecar final
        {
            std::vector<std::vector<std::uint32_t>> event_handlers;
        };

        struct HandlerRange final
        {
            std::uint32_t offset{};
            std::uint32_t count{};
        };

        struct Occurrence final
        {
            ScriptEventSlot event;
            ecs::Entity target{ecs::NullEntity};
            const lux_script_value_slot* args{};
            lux_script_value_slot* returns{};
            std::uint32_t arg_count{};
            std::uint32_t return_count{};
            bool dispatched{};

            [[nodiscard]] lux_script_call_frame makeFrame() const noexcept
            {
                return lux_script_call_frame{
                    args,
                    arg_count,
                    0U,
                    returns,
                    return_count,
                    0U,
                    nullptr,
                    nullptr};
            }
        };

        struct BuildState final
        {
            std::vector<Prepared> prepared;
            std::vector<std::vector<std::uint32_t>> hook_handlers;
            std::vector<std::vector<std::uint32_t>> global_event_handlers;
            std::vector<EntitySidecar> sidecars;
            std::vector<std::uint32_t> entity_to_sidecar;
            std::vector<HandlerRange> entity_event_ranges;
            std::vector<std::uint32_t> entity_event_handlers;
        };

        State(
            SimulationDescription source,
            ecs::Registry& source_registry,
            ScriptBindingCapacities source_capacities,
            ScriptAssetResolver source_resolver,
            std::span<const ScriptBackendDescriptor> source_backends,
            ScriptHostApi source_host_api
        )
            : description(std::move(source)),
              registry(std::addressof(source_registry)),
              capacities(source_capacities),
              resolver(source_resolver),
              host_api(source_host_api),
              constructed(source_registry.on_construct<ScriptMountFacts>()
                  .connect<&State::onMountsChanged>(*this)),
              updated(source_registry.on_update<ScriptMountFacts>()
                  .connect<&State::onMountsChanged>(*this)),
              destroyed(source_registry.on_destroy<ScriptMountFacts>()
                  .connect<&State::onMountsChanged>(*this))
        {
            backends.assign(source_backends.begin(), source_backends.end());
            prepared.reserve(capacities.prepared_calls);
            failures.reserve(capacities.failures);
            occurrences.resize(capacities.producer_count);
            for (auto& producer : occurrences)
                producer.reserve(capacities.occurrences_per_producer);
            makeSlots();
        }

        ~State()
        {
            releasePrepared(prepared);
        }

        void onMountsChanged(ecs::Registry&, ecs::Entity) noexcept
        {
            mounts_dirty = true;
        }

        void makeSlots()
        {
            for (std::size_t system_index{};
                 system_index < description.systemCount();
                 ++system_index)
            {
                const auto system = description.systemAt(system_index);
                for (std::size_t hook_index{};
                     hook_index < system.hookPointCount();
                     ++hook_index)
                {
                    hooks.push_back(Hook{
                        system_index,
                        hook_index,
                        system.hookPointAt(hook_index).cardinality(),
                        {}});
                }
            }
            for (std::size_t system_index{};
                 system_index < description.systemCount();
                 ++system_index)
            {
                const auto system = description.systemAt(system_index);
                for (std::size_t event_index{};
                     event_index < system.eventCount();
                     ++event_index)
                {
                    const auto event = system.eventAt(event_index);
                    events.push_back(Event{
                        system_index,
                        event_index,
                        findHookSlot(system_index, event.dispatchHook().name()),
                        event.target(),
                        {}});
                }
            }
        }

        [[nodiscard]] ScriptHookSlot findHookSlot(
            std::size_t system_index,
            std::string_view name
        ) const noexcept
        {
            for (std::size_t index{}; index < hooks.size(); ++index)
            {
                const auto& hook = hooks[index];
                if (hook.system == system_index &&
                    description.systemAt(system_index)
                        .hookPointAt(hook.member).name() == name)
                {
                    return ScriptHookSlot{static_cast<std::uint32_t>(index)};
                }
            }
            return {};
        }

        [[nodiscard]] ScriptEventSlot findEventSlot(
            std::size_t system_index,
            std::string_view name
        ) const noexcept
        {
            for (std::size_t index{}; index < events.size(); ++index)
            {
                const auto& event = events[index];
                if (event.system == system_index &&
                    description.systemAt(system_index)
                        .eventAt(event.member).name() == name)
                {
                    return ScriptEventSlot{static_cast<std::uint32_t>(index)};
                }
            }
            return {};
        }

        [[nodiscard]] const ScriptBackendDescriptor* backendFor(
            lux::rdesc::Script::Kind kind,
            std::size_t& index
        ) const noexcept
        {
            for (index = 0U; index < backends.size(); ++index)
            {
                if (backends[index].kind == kind && backends[index].prepare)
                    return std::addressof(backends[index]);
            }
            return nullptr;
        }

        void releasePrepared(std::vector<Prepared>& values) noexcept
        {
            for (auto& value : values)
            {
                if (value.call && value.backend < backends.size())
                {
                    const auto& backend = backends[value.backend];
                    if (backend.release)
                        backend.release(backend.context, value.call);
                }
                value.call = {};
                value.active = false;
            }
            values.clear();
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptBindingError>
        appendBinding(
            BuildState& build,
            const lux::asset::ScriptAssetContent& asset,
            const lux::rdesc::ScriptBindingDescription& binding,
            ecs::Entity entity,
            std::size_t sidecar_index,
            const lux::asset::AssetId& asset_id,
            std::size_t mount_ordinal,
            std::size_t binding_ordinal
        ) noexcept
        {
            if (build.prepared.size() >= capacities.prepared_calls)
            {
                return lux::cxx::unexpected(
                    EScriptBindingError::CAPACITY_EXCEEDED
                );
            }

            const auto export_it = std::find_if(
                asset.description.exports.begin(),
                asset.description.exports.end(),
                [&](const auto& function) noexcept
                {
                    return function.symbol_id == binding.function;
                }
            );
            if (export_it == asset.description.exports.end())
            {
                return lux::cxx::unexpected(
                    EScriptBindingError::SYMBOL_NOT_FOUND
                );
            }

            std::size_t target_system = description.systemCount();
            if (!binding.system_instance.empty())
            {
                for (std::size_t index{}; index < description.systemCount(); ++index)
                {
                    const auto system = description.systemAt(index);
                    if (system.instanceName() == binding.system_instance &&
                        system.type().name == binding.system_type)
                    {
                        target_system = index;
                        break;
                    }
                }
            }
            else
            {
                for (std::size_t index{}; index < description.systemCount(); ++index)
                {
                    if (description.systemAt(index).type().name != binding.system_type)
                        continue;
                    if (target_system != description.systemCount())
                    {
                        return lux::cxx::unexpected(
                            EScriptBindingError::SCOPE_MISMATCH
                        );
                    }
                    target_system = index;
                }
            }
            if (target_system == description.systemCount())
            {
                return lux::cxx::unexpected(
                    EScriptBindingError::SYSTEM_NOT_FOUND
                );
            }

            ScriptHookSlot hook_slot;
            ScriptEventSlot event_slot;
            if (binding.kind == lux::rdesc::EScriptBindingKind::HOOK)
            {
                if (entity != ecs::NullEntity)
                {
                    return lux::cxx::unexpected(
                        EScriptBindingError::SCOPE_MISMATCH
                    );
                }
                hook_slot = findHookSlot(target_system, binding.member);
                if (!hook_slot)
                {
                    return lux::cxx::unexpected(
                        EScriptBindingError::MEMBER_NOT_FOUND
                    );
                }
                const auto hook = description.systemAt(target_system)
                    .findHookPoint(binding.member);
                if (!sameHookSignature(*export_it, hook))
                {
                    return lux::cxx::unexpected(
                        EScriptBindingError::SIGNATURE_MISMATCH
                    );
                }
                if (hook.cardinality() == ESystemHookCardinality::SINGLE &&
                    !build.hook_handlers[hook_slot.value].empty())
                {
                    return lux::cxx::unexpected(
                        EScriptBindingError::SCOPE_MISMATCH
                    );
                }
            }
            else
            {
                event_slot = findEventSlot(target_system, binding.member);
                if (!event_slot)
                {
                    return lux::cxx::unexpected(
                        EScriptBindingError::MEMBER_NOT_FOUND
                    );
                }
                const auto event = description.systemAt(target_system)
                    .findEvent(binding.member);
                if ((entity == ecs::NullEntity) !=
                    (event.target() == ESystemEventTarget::GLOBAL))
                {
                    return lux::cxx::unexpected(
                        EScriptBindingError::SCOPE_MISMATCH
                    );
                }
                if (!sameEventSignature(*export_it, event))
                {
                    return lux::cxx::unexpected(
                        EScriptBindingError::SIGNATURE_MISMATCH
                    );
                }
            }

            std::size_t backend_index{};
            const auto* backend = backendFor(
                asset.description.kind(),
                backend_index
            );
            if (!backend)
            {
                return lux::cxx::unexpected(
                    EScriptBindingError::BACKEND_NOT_FOUND
                );
            }
            lux::script::BoundScriptCall call;
            const auto prepared = backend->prepare(
                backend->context,
                ScriptPrepareContext{
                    asset_id,
                    entity,
                    static_cast<std::uint32_t>(mount_ordinal),
                    static_cast<std::uint32_t>(binding_ordinal)},
                asset,
                *export_it,
                call
            );
            if (prepared != EScriptBackendPrepareResult::SUCCESS || !call)
            {
                const auto error = [&]() noexcept
                {
                    switch (prepared)
                    {
                    case EScriptBackendPrepareResult::CAPACITY_EXCEEDED:
                        return EScriptBindingError::CAPACITY_EXCEEDED;
                    case EScriptBackendPrepareResult::ALLOCATION_FAILURE:
                        return EScriptBindingError::ALLOCATION_FAILURE;
                    case EScriptBackendPrepareResult::SIGNATURE_MISMATCH:
                        return EScriptBindingError::SIGNATURE_MISMATCH;
                    case EScriptBackendPrepareResult::SUCCESS:
                    case EScriptBackendPrepareResult::CONSTRUCTION_FAILURE:
                        return EScriptBindingError::BACKEND_CONSTRUCTION_FAILURE;
                    }
                    return EScriptBindingError::BACKEND_CONSTRUCTION_FAILURE;
                }();
                return lux::cxx::unexpected(
                    error
                );
            }

            const auto prepared_index = static_cast<std::uint32_t>(
                build.prepared.size()
            );
            build.prepared.push_back(Prepared{
                call,
                backend_index,
                binding.function,
                entity,
                true,
                false});
            if (hook_slot)
                build.hook_handlers[hook_slot.value].push_back(prepared_index);
            else if (entity == ecs::NullEntity)
                build.global_event_handlers[event_slot.value].push_back(
                    prepared_index
                );
            else
                build.sidecars[sidecar_index]
                    .event_handlers[event_slot.value].push_back(prepared_index);
            return {};
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptBindingError> appendMount(
            BuildState& build,
            const ScriptMountDescription& mount,
            ecs::Entity entity,
            std::size_t sidecar_index,
            std::size_t mount_ordinal
        ) noexcept
        {
            ResolvedScriptAsset resolved;
            if (!resolver.resolve ||
                !resolver.resolve(resolver.context, mount.script, resolved) ||
                !resolved.asset)
            {
                return lux::cxx::unexpected(
                    EScriptBindingError::ASSET_NOT_FOUND
                );
            }
            const auto release_asset = [&]() noexcept
            {
                if (resolved.release)
                    resolved.release(resolved.lease);
            };
            if (!lux::rdesc::validScriptDescription(
                    resolved.asset->description
                ))
            {
                release_asset();
                return lux::cxx::unexpected(
                    EScriptBindingError::INVALID_ASSET
                );
            }
            const auto& bindings =
                mount.binding_mode == EScriptBindingSetMode::ASSET_DEFAULTS
                ? resolved.asset->description.default_bindings
                : mount.bindings;
            for (std::size_t binding_index{};
                 binding_index < bindings.size();
                 ++binding_index)
            {
                auto result = appendBinding(
                    build,
                    *resolved.asset,
                    bindings[binding_index],
                    entity,
                    sidecar_index,
                    mount.script,
                    mount_ordinal,
                    binding_index
                );
                if (!result)
                {
                    release_asset();
                    return result;
                }
            }
            release_asset();
            return {};
        }

        [[nodiscard]] lux::cxx::expected<BuildState, EScriptBindingError>
        buildPrepared() noexcept
        {
            BuildState build;
            try
            {
                build.prepared.reserve(capacities.prepared_calls);
                build.hook_handlers.resize(hooks.size());
                build.global_event_handlers.resize(events.size());
                build.entity_to_sidecar.assign(
                    capacities.entity_slots,
                    kInvalidIndex
                );

                for (std::size_t mount_index{};
                     mount_index < description.globalScriptMountCount();
                     ++mount_index)
                {
                    const auto view = description.globalScriptMountAt(mount_index);
                    ScriptMountDescription mount;
                    mount.script = view.script();
                    mount.binding_mode = view.bindingMode();
                    mount.bindings.reserve(view.bindingCount());
                    for (std::size_t index{}; index < view.bindingCount(); ++index)
                        mount.bindings.push_back(*view.bindingAt(index));
                    auto result = appendMount(
                        build,
                        mount,
                        ecs::NullEntity,
                        0U,
                        mount_index
                    );
                    if (!result)
                    {
                        releasePrepared(build.prepared);
                        return lux::cxx::unexpected(result.error());
                    }
                }

                std::vector<ecs::Entity> entities;
                entities.reserve(capacities.entity_slots);
                for (const auto entity : registry->view<ScriptMountFacts>())
                {
                    if (entityIndex(entity) >= capacities.entity_slots ||
                        entities.size() >= capacities.entity_slots)
                    {
                        releasePrepared(build.prepared);
                        return lux::cxx::unexpected(
                            EScriptBindingError::CAPACITY_EXCEEDED
                        );
                    }
                    entities.push_back(entity);
                }
                std::sort(
                    entities.begin(),
                    entities.end(),
                    [](ecs::Entity left, ecs::Entity right) noexcept
                    {
                        return ecs::entityBits(left) < ecs::entityBits(right);
                    }
                );
                build.sidecars.reserve(entities.size());
                for (const auto entity : entities)
                {
                    const auto sidecar_index = build.sidecars.size();
                    build.sidecars.push_back(EntitySidecar{
                        std::vector<std::vector<std::uint32_t>>(events.size())});
                    build.entity_to_sidecar[entityIndex(entity)] =
                        static_cast<std::uint32_t>(sidecar_index);
                    const auto& facts = registry->get<ScriptMountFacts>(entity);
                    for (std::size_t mount_index{};
                         mount_index < facts.mounts.size();
                         ++mount_index)
                    {
                        auto result = appendMount(
                            build,
                            facts.mounts[mount_index],
                            entity,
                            sidecar_index,
                            mount_index
                        );
                        if (!result)
                        {
                            releasePrepared(build.prepared);
                            return lux::cxx::unexpected(result.error());
                        }
                    }
                }

                if (!events.empty() &&
                    build.sidecars.size() >
                        std::numeric_limits<std::size_t>::max() / events.size())
                {
                    releasePrepared(build.prepared);
                    return lux::cxx::unexpected(
                        EScriptBindingError::CAPACITY_EXCEEDED
                    );
                }
                build.entity_event_ranges.reserve(
                    build.sidecars.size() * events.size()
                );
                build.entity_event_handlers.reserve(build.prepared.size());
                for (std::size_t event_index{};
                     event_index < events.size();
                     ++event_index)
                {
                    for (const auto& sidecar : build.sidecars)
                    {
                        const auto& handlers =
                            sidecar.event_handlers[event_index];
                        if (build.entity_event_handlers.size() >
                                std::numeric_limits<std::uint32_t>::max() ||
                            handlers.size() >
                                std::numeric_limits<std::uint32_t>::max() ||
                            handlers.size() >
                                std::numeric_limits<std::uint32_t>::max() -
                                    build.entity_event_handlers.size())
                        {
                            releasePrepared(build.prepared);
                            return lux::cxx::unexpected(
                                EScriptBindingError::CAPACITY_EXCEEDED
                            );
                        }
                        build.entity_event_ranges.push_back(HandlerRange{
                            static_cast<std::uint32_t>(
                                build.entity_event_handlers.size()
                            ),
                            static_cast<std::uint32_t>(handlers.size())});
                        build.entity_event_handlers.insert(
                            build.entity_event_handlers.end(),
                            handlers.begin(),
                            handlers.end()
                        );
                    }
                }
                return build;
            }
            catch (const std::bad_alloc&)
            {
                releasePrepared(build.prepared);
                return lux::cxx::unexpected(
                    EScriptBindingError::ALLOCATION_FAILURE
                );
            }
        }

        void accept(BuildState build) noexcept
        {
            releasePrepared(prepared);
            prepared = std::move(build.prepared);
            for (std::size_t index{}; index < hooks.size(); ++index)
                hooks[index].handlers = std::move(build.hook_handlers[index]);
            for (std::size_t index{}; index < events.size(); ++index)
            {
                events[index].global_handlers =
                    std::move(build.global_event_handlers[index]);
            }
            entity_to_sidecar = std::move(build.entity_to_sidecar);
            entity_sidecar_count = build.sidecars.size();
            entity_event_ranges = std::move(build.entity_event_ranges);
            entity_event_handlers = std::move(build.entity_event_handlers);
            mounts_dirty = false;
            prepared_once = true;
        }

        SimulationDescription description;
        ecs::Registry* registry{};
        ScriptBindingCapacities capacities;
        ScriptAssetResolver resolver;
        ScriptHostApi host_api;
        std::vector<ScriptBackendDescriptor> backends;
        std::vector<Hook> hooks;
        std::vector<Event> events;
        std::vector<Prepared> prepared;
        std::vector<std::uint32_t> entity_to_sidecar;
        std::size_t entity_sidecar_count{};
        std::vector<HandlerRange> entity_event_ranges;
        std::vector<std::uint32_t> entity_event_handlers;
        std::vector<std::vector<Occurrence>> occurrences;
        std::vector<ScriptBindingFailure> failures;
        entt::scoped_connection constructed;
        entt::scoped_connection updated;
        entt::scoped_connection destroyed;
        bool mounts_dirty{true};
        bool prepared_once{};
    };

    ScriptEventWriter::ScriptEventWriter(
        ScriptBindingSession& session,
        std::size_t producer
    ) noexcept
        : session_(std::addressof(session)), producer_(producer)
    {}

    lux::cxx::expected<void, EScriptBindingError> ScriptEventWriter::emit(
        ScriptEventSlot event,
        const lux_script_call_frame& frame
    ) noexcept
    {
        return session_
            ? session_->emit(producer_, event, ecs::NullEntity, frame)
            : lux::cxx::expected<void, EScriptBindingError>{
                lux::cxx::unexpected(EScriptBindingError::INVALID_PRODUCER)};
    }

    lux::cxx::expected<void, EScriptBindingError> ScriptEventWriter::emit(
        ScriptEventSlot event,
        ecs::Entity target,
        const lux_script_call_frame& frame
    ) noexcept
    {
        return session_
            ? session_->emit(producer_, event, target, frame)
            : lux::cxx::expected<void, EScriptBindingError>{
                lux::cxx::unexpected(EScriptBindingError::INVALID_PRODUCER)};
    }

    ScriptBindingSession::ScriptBindingSession(
        std::unique_ptr<State> state
    ) noexcept
        : state_(std::move(state))
    {}

    lux::cxx::expected<ScriptBindingSession, EScriptBindingError>
    ScriptBindingSession::create(
        SimulationDescription description,
        ecs::Registry& registry,
        ScriptBindingCapacities capacities,
        ScriptAssetResolver resolver,
        std::span<const ScriptBackendDescriptor> backends,
        ScriptHostApi host_api
    ) noexcept
    {
        if (!resolver.resolve || capacities.prepared_calls == 0U ||
            capacities.producer_count == 0U ||
            capacities.occurrences_per_producer == 0U)
        {
            return lux::cxx::unexpected(
                EScriptBindingError::CAPACITY_EXCEEDED
            );
        }
        try
        {
            return ScriptBindingSession(std::make_unique<State>(
                std::move(description),
                registry,
                capacities,
                resolver,
                backends,
                host_api
            ));
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(
                EScriptBindingError::ALLOCATION_FAILURE
            );
        }
    }

    ScriptBindingSession::ScriptBindingSession(ScriptBindingSession&&) noexcept =
        default;
    ScriptBindingSession& ScriptBindingSession::operator=(
        ScriptBindingSession&&
    ) noexcept = default;
    ScriptBindingSession::~ScriptBindingSession() = default;

    lux::cxx::expected<void, EScriptBindingError>
    ScriptBindingSession::prepare() noexcept
    {
        if (!state_)
            return lux::cxx::unexpected(EScriptBindingError::INVALID_SLOT);
        auto build = state_->buildPrepared();
        if (!build)
            return lux::cxx::unexpected(build.error());
        state_->accept(std::move(*build));
        return {};
    }

    lux::cxx::expected<void, EScriptBindingError>
    ScriptBindingSession::applyQuiescentMutations() noexcept
    {
        if (!state_)
            return lux::cxx::unexpected(EScriptBindingError::INVALID_SLOT);
        if (state_->mounts_dirty)
            return prepare();
        for (auto& prepared : state_->prepared)
        {
            if (!prepared.pending_disable || !prepared.active)
                continue;
            const auto& backend = state_->backends[prepared.backend];
            if (backend.release)
                backend.release(backend.context, prepared.call);
            prepared.call = {};
            prepared.active = false;
            prepared.pending_disable = false;
        }
        return {};
    }

    void ScriptBindingSession::beginUpdate() noexcept
    {
        if (!state_)
            return;
        for (auto& producer : state_->occurrences)
            producer.clear();
        state_->failures.clear();
    }

    ScriptHookSlot ScriptBindingSession::hookSlot(
        std::string_view system_instance,
        std::string_view hook_name
    ) const noexcept
    {
        if (!state_)
            return {};
        for (std::size_t index{}; index < state_->description.systemCount(); ++index)
        {
            if (state_->description.systemAt(index).instanceName() == system_instance)
                return state_->findHookSlot(index, hook_name);
        }
        return {};
    }

    ScriptEventSlot ScriptBindingSession::eventSlot(
        std::string_view system_instance,
        std::string_view event_name
    ) const noexcept
    {
        if (!state_)
            return {};
        for (std::size_t index{}; index < state_->description.systemCount(); ++index)
        {
            if (state_->description.systemAt(index).instanceName() == system_instance)
                return state_->findEventSlot(index, event_name);
        }
        return {};
    }

    ScriptEventWriter ScriptBindingSession::writer(std::size_t producer) noexcept
    {
        return ScriptEventWriter(*this, producer);
    }

    lux::cxx::expected<void, EScriptBindingError> ScriptBindingSession::emit(
        std::size_t producer,
        ScriptEventSlot event,
        ecs::Entity target,
        const lux_script_call_frame& frame
    ) noexcept
    {
        if (!state_ || producer >= state_->occurrences.size())
            return lux::cxx::unexpected(EScriptBindingError::INVALID_PRODUCER);
        if (!event || event.value >= state_->events.size())
            return lux::cxx::unexpected(EScriptBindingError::INVALID_SLOT);
        const auto& metadata = state_->events[event.value];
        if ((target == ecs::NullEntity) !=
            (metadata.target == ESystemEventTarget::GLOBAL))
        {
            return lux::cxx::unexpected(EScriptBindingError::INVALID_ENTITY);
        }
        if (target != ecs::NullEntity &&
            (entityIndex(target) >= state_->entity_to_sidecar.size() ||
             state_->entity_to_sidecar[entityIndex(target)] == kInvalidIndex))
        {
            return lux::cxx::unexpected(EScriptBindingError::INVALID_ENTITY);
        }
        auto& buffer = state_->occurrences[producer];
        if (buffer.size() >= state_->capacities.occurrences_per_producer)
        {
            return lux::cxx::unexpected(
                EScriptBindingError::OCCURRENCE_CAPACITY_EXCEEDED
            );
        }
        buffer.push_back(State::Occurrence{
            event,
            target,
            frame.args,
            frame.returns,
            frame.arg_count,
            frame.return_count,
            false});
        return {};
    }

    ScriptDispatchResult ScriptBindingSession::dispatchHook(
        ScriptHookSlot hook,
        const lux_script_call_frame& source_frame
    ) noexcept
    {
        ScriptDispatchResult result;
        if (!state_ || !hook || hook.value >= state_->hooks.size())
        {
            result.status = -1;
            result.failures = 1U;
            return result;
        }
        const auto invoke = [&](
            std::uint32_t prepared_index,
            lux_script_call_frame& frame,
            ecs::Entity self,
            bool single
        ) noexcept -> bool
        {
            if (prepared_index >= state_->prepared.size())
                return true;
            auto& prepared = state_->prepared[prepared_index];
            if (!prepared.active || !prepared.call)
                return true;
            ScriptHostContext host{std::addressof(state_->host_api), self};
            frame.world_context = std::addressof(host);
            frame.user_context = prepared.call.context;
            const auto status = prepared.call.invoke(std::addressof(frame));
            ++result.calls;
            if (status == 0)
                return true;
            ++result.failures;
            result.status = status;
            prepared.pending_disable = true;
            if (state_->failures.size() < state_->capacities.failures)
            {
                state_->failures.push_back(ScriptBindingFailure{
                    EScriptBindingError::INVOCATION_FAILURE,
                    prepared.symbol,
                    self,
                    status});
            }
            return !single;
        };

        // Producer ordinal is the outer loop and append order is the inner
        // loop: this is the canonical deterministic merge order.
        for (auto& producer : state_->occurrences)
        {
            for (auto& occurrence : producer)
            {
                if (occurrence.dispatched ||
                    state_->events[occurrence.event.value].dispatch_hook != hook)
                {
                    continue;
                }
                occurrence.dispatched = true;
                const auto& event = state_->events[occurrence.event.value];
                auto event_frame = occurrence.makeFrame();
                if (occurrence.target == ecs::NullEntity)
                {
                    for (const auto handler : event.global_handlers)
                    {
                        (void)invoke(
                            handler,
                            event_frame,
                            ecs::NullEntity,
                            false
                        );
                    }
                }
                else
                {
                    const auto sidecar = state_->entity_to_sidecar[
                        entityIndex(occurrence.target)
                    ];
                    const auto range_index =
                        static_cast<std::size_t>(occurrence.event.value) *
                            state_->entity_sidecar_count +
                        sidecar;
                    if (range_index >= state_->entity_event_ranges.size())
                        continue;
                    const auto range = state_->entity_event_ranges[range_index];
                    for (std::size_t index{}; index < range.count; ++index)
                    {
                        const auto handler = state_->entity_event_handlers[
                            static_cast<std::size_t>(range.offset) + index
                        ];
                        (void)invoke(
                            handler,
                            event_frame,
                            occurrence.target,
                            false
                        );
                    }
                }
            }
        }

        // Events are completely delivered before the hook handlers start.
        auto hook_frame = source_frame;
        const auto& hook_metadata = state_->hooks[hook.value];
        for (const auto handler : hook_metadata.handlers)
        {
            if (!invoke(
                    handler,
                    hook_frame,
                    ecs::NullEntity,
                    hook_metadata.cardinality == ESystemHookCardinality::SINGLE
                ))
            {
                break;
            }
        }
        return result;
    }

    std::span<const ScriptBindingFailure> ScriptBindingSession::failures() const
        noexcept
    {
        return state_ ? std::span<const ScriptBindingFailure>{state_->failures}
                      : std::span<const ScriptBindingFailure>{};
    }

    std::size_t ScriptBindingSession::preparedCallCount() const noexcept
    {
        return state_ ? state_->prepared.size() : 0U;
    }

    std::size_t ScriptBindingSession::pendingOccurrenceCount() const noexcept
    {
        if (!state_)
            return 0U;
        std::size_t result{};
        for (const auto& producer : state_->occurrences)
            result += producer.size();
        return result;
    }

    std::size_t ScriptBindingSession::hotPathNameLookupCount() const noexcept
    {
        return 0U;
    }

    std::size_t ScriptBindingSession::hotPathAssetLookupCount() const noexcept
    {
        return 0U;
    }

    std::size_t ScriptBindingSession::hotPathSceneScanCount() const noexcept
    {
        return 0U;
    }
}
