#include <lux/engine/simulation/script/ScriptSystem.hpp>

#include <algorithm>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include <entt/signal/sigh.hpp>

namespace lux::simulation::script
{
    namespace
    {
        enum class EInstanceState : std::uint8_t
        {
            CONSTRUCTING,
            ACTIVE,
            RETIRING,
            DEAD,
        };

        [[nodiscard]] bool sameType(
            const lux::rdesc::ScriptValueType& script_type,
            const lux::semantic::Type& endpoint_type
        ) noexcept
        {
            return script_type.type_id == endpoint_type.type_id &&
                script_type.canonical_name == endpoint_type.canonical_name &&
                script_type.pass == endpoint_type.pass;
        }

        [[nodiscard]] bool sameHookSignature(
            const lux::rdesc::ScriptFunction& function,
            lux::semantic::SignatureView signature
        ) noexcept
        {
            if (!function.returns.empty() ||
                function.args.size() != signature.parameters.size())
            {
                return false;
            }
            for (std::size_t index{}; index < function.args.size(); ++index)
            {
                if (!sameType(function.args[index], signature.parameters[index]))
                    return false;
            }
            return true;
        }

        [[nodiscard]] bool sameEventSignature(
            const lux::rdesc::ScriptFunction& function,
            const lux::semantic::Type& payload
        ) noexcept
        {
            return function.returns.empty() && function.args.size() == 1U &&
                sameType(function.args.front(), payload);
        }
    }

    struct ScriptSystem::State final
    {
        struct PreparedMethod final
        {
            lux::script::ScriptSymbolId symbol{};
            lux::script::BoundScriptCall call;
        };

        struct Instance final
        {
            const ScriptMountDescription* authored{};
            ScriptInstanceScope scope;
            ScriptBehavior behavior;
            ResolvedScriptAsset asset;
            const ScriptBackendDescriptor* backend{};
            ScriptBackendInstance backend_instance;
            std::vector<PreparedMethod> methods;
            EInstanceState state{EInstanceState::DEAD};
            EBehaviorStopReason stop_reason{
                EBehaviorStopReason::SIMULATION_STOPPED};
        };

        struct Handler final
        {
            Instance* instance{};
            lux::script::BoundScriptCall call;
            ScriptMountId mount;
            std::size_t binding_ordinal{};
            ecs::Entity target{ecs::NullEntity};
        };

        struct HookBucket final
        {
            State* owner{};
            const ScriptHookEndpointDescriptor* endpoint{};
            EndpointConnectionToken token;
            std::vector<Handler> handlers;
        };

        struct EntityRange final
        {
            ecs::Entity entity{ecs::NullEntity};
            std::size_t first{};
            std::size_t count{};
        };

        struct EventBucket final
        {
            State* owner{};
            const ScriptEventEndpointDescriptor* endpoint{};
            EndpointConnectionToken token;
            std::vector<Handler> handlers;
            std::vector<EntityRange> ranges;
        };

        const SimulationDescription* simulation{};
        const ScriptSystemDescription* description{};
        ecs::Registry* registry{};
        ScriptSystemCapacities capacities;
        ResidentScriptResolver assets;
        ScriptWorldResolver world;
        ScriptHostApi host;
        std::vector<ScriptBackendDescriptor> backends;
        std::vector<ScriptHookEndpointDescriptor> hook_endpoints;
        std::vector<ScriptEventEndpointDescriptor> event_endpoints;
        std::vector<Instance> instances;
        std::vector<HookBucket> hooks;
        std::vector<EventBucket> events;
        std::vector<ScriptSystemFailure> failures;
        std::vector<ecs::Entity> dirty_current;
        std::vector<ecs::Entity> dirty_processing;
        entt::connection constructed;
        entt::connection updated;
        entt::connection destroyed;
        bool prepared{};
        bool shut_down{};
        bool full_resync{};

        [[nodiscard]] const ScriptBackendDescriptor* findBackend(
            lux::rdesc::Script::Kind kind
        ) const noexcept
        {
            const auto found = std::find_if(
                backends.begin(),
                backends.end(),
                [kind](const auto& backend) noexcept
                {
                    return backend.kind == kind;
                }
            );
            return found == backends.end() ? nullptr : std::addressof(*found);
        }

        [[nodiscard]] const ScriptHookEndpointDescriptor* findHook(
            HookScriptTarget target
        ) const noexcept
        {
            const auto found = std::find_if(
                hook_endpoints.begin(),
                hook_endpoints.end(),
                [&](const auto& endpoint) noexcept
                {
                    return endpoint.system == target.system &&
                        endpoint.hook == target.hook;
                }
            );
            return found == hook_endpoints.end()
                ? nullptr
                : std::addressof(*found);
        }

        [[nodiscard]] const ScriptEventEndpointDescriptor* findEvent(
            EventScriptTarget target
        ) const noexcept
        {
            const auto found = std::find_if(
                event_endpoints.begin(),
                event_endpoints.end(),
                [&](const auto& endpoint) noexcept
                {
                    return endpoint.system == target.system &&
                        endpoint.event == target.event;
                }
            );
            return found == event_endpoints.end()
                ? nullptr
                : std::addressof(*found);
        }

        [[nodiscard]] HookBucket* findHookBucket(
            const ScriptHookEndpointDescriptor* endpoint
        ) noexcept
        {
            const auto found = std::find_if(
                hooks.begin(),
                hooks.end(),
                [&](const auto& bucket) noexcept
                {
                    return bucket.endpoint == endpoint;
                }
            );
            return found == hooks.end() ? nullptr : std::addressof(*found);
        }

        [[nodiscard]] EventBucket* findEventBucket(
            const ScriptEventEndpointDescriptor* endpoint
        ) noexcept
        {
            const auto found = std::find_if(
                events.begin(),
                events.end(),
                [&](const auto& bucket) noexcept
                {
                    return bucket.endpoint == endpoint;
                }
            );
            return found == events.end() ? nullptr : std::addressof(*found);
        }

        [[nodiscard]] PreparedMethod* findMethod(
            Instance& instance,
            lux::script::ScriptSymbolId symbol
        ) noexcept
        {
            const auto found = std::find_if(
                instance.methods.begin(),
                instance.methods.end(),
                [symbol](const auto& method) noexcept
                {
                    return method.symbol == symbol;
                }
            );
            return found == instance.methods.end()
                ? nullptr
                : std::addressof(*found);
        }

        void recordFailure(
            EScriptSystemError error,
            Instance& instance,
            lux::script::ScriptSymbolId symbol = 0U,
            std::int32_t status = 0
        ) noexcept
        {
            if (failures.size() < capacities.failures)
                failures.push_back({error, instance.authored->id, symbol, status});
        }

        void invoke(
            Handler& handler,
            lux_script_call_frame& frame
        ) noexcept
        {
            if (handler.instance->state != EInstanceState::ACTIVE)
                return;
            frame.user_context = handler.call.context;
            const auto status = handler.call.invoke(&frame);
            if (status == 0)
                return;
            handler.instance->state = EInstanceState::RETIRING;
            handler.instance->stop_reason =
                EBehaviorStopReason::INVOCATION_FAILED;
            recordFailure(
                EScriptSystemError::INVOCATION_FAILURE,
                *handler.instance,
                0U,
                status
            );
        }

        static void invokeHookLane(
            void* context,
            lux_script_call_frame& frame
        ) noexcept
        {
            auto& bucket = *static_cast<HookBucket*>(context);
            for (auto& handler : bucket.handlers)
                bucket.owner->invoke(handler, frame);
        }

        static void dispatchEvent(
            void* context,
            ecs::Entity entity,
            lux_script_call_frame& frame
        ) noexcept
        {
            auto& bucket = *static_cast<EventBucket*>(context);
            if (bucket.endpoint->route == EEventRoute::SIMULATION_BROADCAST)
            {
                for (auto& handler : bucket.handlers)
                    bucket.owner->invoke(handler, frame);
                return;
            }
            const auto found = std::lower_bound(
                bucket.ranges.begin(),
                bucket.ranges.end(),
                entity,
                [](const EntityRange& range, ecs::Entity value) noexcept
                {
                    return ecs::entityBits(range.entity) <
                        ecs::entityBits(value);
                }
            );
            if (found == bucket.ranges.end() || found->entity != entity)
                return;
            for (std::size_t index{}; index < found->count; ++index)
                bucket.owner->invoke(
                    bucket.handlers[found->first + index],
                    frame
                );
        }

        void rebuildRanges(EventBucket& bucket)
        {
            bucket.ranges.clear();
            if (bucket.endpoint->route != EEventRoute::ENTITY_TARGETED)
                return;
            std::size_t first{};
            while (first < bucket.handlers.size())
            {
                std::size_t end{first + 1U};
                while (end < bucket.handlers.size() &&
                       bucket.handlers[end].target ==
                           bucket.handlers[first].target)
                {
                    ++end;
                }
                bucket.ranges.push_back({
                    bucket.handlers[first].target,
                    first,
                    end - first});
                first = end;
            }
        }

        void sortHandlers()
        {
            const auto by_mount = [](const Handler& left, const Handler& right)
            {
                if (left.mount != right.mount)
                    return left.mount < right.mount;
                return left.binding_ordinal < right.binding_ordinal;
            };
            for (auto& bucket : hooks)
                std::sort(bucket.handlers.begin(), bucket.handlers.end(), by_mount);
            for (auto& bucket : events)
            {
                if (bucket.endpoint->route == EEventRoute::ENTITY_TARGETED)
                {
                    std::sort(
                        bucket.handlers.begin(),
                        bucket.handlers.end(),
                        [&](const Handler& left, const Handler& right)
                        {
                            const auto left_bits = ecs::entityBits(left.target);
                            const auto right_bits = ecs::entityBits(right.target);
                            return left_bits != right_bits
                                ? left_bits < right_bits
                                : by_mount(left, right);
                        }
                    );
                }
                else
                {
                    std::sort(
                        bucket.handlers.begin(),
                        bucket.handlers.end(),
                        by_mount
                    );
                }
                rebuildRanges(bucket);
            }
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptSystemError>
        ensureBuckets(const ScriptMountDescription& mount) noexcept
        {
            for (const auto& binding : mount.bindings)
            {
                if (const auto* target =
                        std::get_if<HookScriptTarget>(&binding.target))
                {
                    const auto* endpoint = findHook(*target);
                    if (!endpoint)
                        return lux::cxx::unexpected(
                            EScriptSystemError::ENDPOINT_NOT_FOUND);
                    if (!findHookBucket(endpoint))
                    {
                        if (hooks.size() >= capacities.hook_buckets)
                            return lux::cxx::unexpected(
                                EScriptSystemError::CAPACITY_EXCEEDED);
                        hooks.push_back({this, endpoint});
                        hooks.back().handlers.reserve(capacities.handlers);
                    }
                }
                else
                {
                    const auto event_target =
                        std::get<EventScriptTarget>(binding.target);
                    const auto* endpoint = findEvent(event_target);
                    if (!endpoint)
                        return lux::cxx::unexpected(
                            EScriptSystemError::ENDPOINT_NOT_FOUND);
                    if (!findEventBucket(endpoint))
                    {
                        if (events.size() >= capacities.event_buckets)
                            return lux::cxx::unexpected(
                                EScriptSystemError::CAPACITY_EXCEEDED);
                        events.push_back({this, endpoint});
                        events.back().handlers.reserve(capacities.handlers);
                        events.back().ranges.reserve(capacities.instances);
                    }
                }
            }
            return {};
        }

        [[nodiscard]] Instance* allocateInstance() noexcept
        {
            const auto dead = std::find_if(
                instances.begin(),
                instances.end(),
                [](const auto& instance) noexcept
                {
                    return instance.state == EInstanceState::DEAD;
                }
            );
            if (dead != instances.end())
                return std::addressof(*dead);
            if (instances.size() >= capacities.instances)
                return nullptr;
            instances.emplace_back();
            return std::addressof(instances.back());
        }

        void releaseInstance(
            Instance& instance,
            EBehaviorStopReason reason
        ) noexcept
        {
            if (instance.backend && instance.backend_instance)
            {
                if (instance.backend->stopInstance)
                {
                    instance.backend->stopInstance(
                        instance.backend->context,
                        instance.backend_instance,
                        reason
                    );
                }
                for (auto iterator = instance.methods.rbegin();
                     iterator != instance.methods.rend(); ++iterator)
                {
                    instance.backend->releaseMethod(
                        instance.backend->context,
                        instance.backend_instance,
                        iterator->call
                    );
                }
                instance.methods.clear();
                instance.backend->destroyInstance(
                    instance.backend->context,
                    instance.backend_instance
                );
            }
            if (instance.asset.lease && instance.asset.release)
                instance.asset.release(instance.asset.lease);
            instance = Instance{};
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptSystemError>
        attachMount(
            const ScriptMountDescription& mount,
            std::optional<ecs::Entity> forced_entity = std::nullopt
        ) noexcept
        {
            if (!mount.enabled)
                return {};
            if (std::any_of(
                    instances.begin(),
                    instances.end(),
                    [&](const auto& instance) noexcept
                    {
                        return instance.state != EInstanceState::DEAD &&
                            instance.authored == std::addressof(mount);
                    }))
            {
                return {};
            }
            auto* instance = allocateInstance();
            if (!instance)
                return lux::cxx::unexpected(
                    EScriptSystemError::CAPACITY_EXCEEDED);
            instance->authored = std::addressof(mount);
            instance->state = EInstanceState::CONSTRUCTING;
            if (std::holds_alternative<SimulationScriptMount>(mount.scope))
            {
                instance->scope = SimulationScriptScope{};
            }
            else
            {
                ecs::Entity entity{ecs::NullEntity};
                const auto& object =
                    std::get<EntityScriptMount>(mount.scope).object;
                if (forced_entity)
                    entity = *forced_entity;
                else if (!world.resolve ||
                         !world.resolve(world.context, object, entity))
                {
                    instance->state = EInstanceState::DEAD;
                    return lux::cxx::unexpected(
                        EScriptSystemError::WORLD_OBJECT_NOT_RESOLVED);
                }
                if (entity == ecs::NullEntity || !registry->valid(entity))
                {
                    instance->state = EInstanceState::DEAD;
                    return lux::cxx::unexpected(
                        EScriptSystemError::WORLD_OBJECT_NOT_RESOLVED);
                }
                instance->scope = EntityScriptScope{entity};
                registry->emplace_or_replace<detail::ScriptAttachment>(
                    entity,
                    object
                );
            }
            instance->behavior.attach(instance->scope, host);

            if (!assets.resolve ||
                !assets.resolve(assets.context, mount.asset, instance->asset))
            {
                instance->state = EInstanceState::DEAD;
                return lux::cxx::unexpected(
                    EScriptSystemError::ASSET_NOT_RESIDENT);
            }
            if (!instance->asset.asset ||
                !lux::rdesc::validScriptDescription(
                    instance->asset.asset->description))
            {
                releaseInstance(
                    *instance,
                    EBehaviorStopReason::INITIALIZATION_FAILED
                );
                return lux::cxx::unexpected(EScriptSystemError::INVALID_ASSET);
            }
            instance->backend = findBackend(
                instance->asset.asset->description.kind());
            if (!instance->backend)
            {
                releaseInstance(
                    *instance,
                    EBehaviorStopReason::INITIALIZATION_FAILED
                );
                return lux::cxx::unexpected(
                    EScriptSystemError::BACKEND_NOT_AVAILABLE);
            }
            const ScriptInstanceCreateContext create_context{
                mount.asset,
                mount.id,
                instance->scope,
                std::addressof(instance->behavior)};
            if (instance->backend->createInstance(
                    instance->backend->context,
                    create_context,
                    *instance->asset.asset,
                    instance->backend_instance) != EScriptBackendResult::SUCCESS)
            {
                releaseInstance(
                    *instance,
                    EBehaviorStopReason::INITIALIZATION_FAILED
                );
                return lux::cxx::unexpected(
                    EScriptSystemError::BACKEND_FAILURE);
            }

            std::vector<lux::script::ScriptSymbolId> symbols;
            symbols.reserve(mount.bindings.size());
            for (const auto& binding : mount.bindings)
            {
                if (std::find(symbols.begin(), symbols.end(), binding.symbol) ==
                    symbols.end())
                {
                    symbols.push_back(binding.symbol);
                }
            }
            std::size_t total_methods{};
            for (const auto& value : instances)
                total_methods += value.methods.size();
            if (total_methods + symbols.size() > capacities.prepared_methods)
            {
                releaseInstance(
                    *instance,
                    EBehaviorStopReason::INITIALIZATION_FAILED
                );
                return lux::cxx::unexpected(
                    EScriptSystemError::CAPACITY_EXCEEDED);
            }
            instance->methods.reserve(symbols.size());
            for (const auto symbol : symbols)
            {
                const auto& exports = instance->asset.asset->description.exports;
                const auto function = std::find_if(
                    exports.begin(),
                    exports.end(),
                    [symbol](const auto& value) noexcept
                    {
                        return value.symbol_id == symbol;
                    }
                );
                if (function == exports.end())
                {
                    releaseInstance(
                        *instance,
                        EBehaviorStopReason::INITIALIZATION_FAILED
                    );
                    return lux::cxx::unexpected(
                        EScriptSystemError::SYMBOL_NOT_FOUND);
                }
                lux::script::BoundScriptCall call;
                if (instance->backend->prepareMethod(
                        instance->backend->context,
                        instance->backend_instance,
                        *function,
                        call) != EScriptBackendResult::SUCCESS || !call)
                {
                    releaseInstance(
                        *instance,
                        EBehaviorStopReason::INITIALIZATION_FAILED
                    );
                    return lux::cxx::unexpected(
                        EScriptSystemError::BACKEND_FAILURE);
                }
                instance->methods.push_back({symbol, call});
            }

            const auto rollback = [&]() noexcept
            {
                removeHandlers(*instance);
                releaseInstance(
                    *instance,
                    EBehaviorStopReason::INITIALIZATION_FAILED
                );
            };

            for (std::size_t ordinal{};
                 ordinal < mount.bindings.size(); ++ordinal)
            {
                const auto& binding = mount.bindings[ordinal];
                auto* method = findMethod(*instance, binding.symbol);
                const auto& exports = instance->asset.asset->description.exports;
                const auto function = std::find_if(
                    exports.begin(),
                    exports.end(),
                    [&](const auto& value) noexcept
                    {
                        return value.symbol_id == binding.symbol;
                    }
                );
                if (const auto* target =
                        std::get_if<HookScriptTarget>(&binding.target))
                {
                    auto* bucket = findHookBucket(findHook(*target));
                    if (!bucket || !sameHookSignature(
                            *function,
                            bucket->endpoint->signature))
                    {
                        rollback();
                        return lux::cxx::unexpected(
                            EScriptSystemError::SIGNATURE_MISMATCH);
                    }
                    if (bucket->handlers.size() >= capacities.handlers)
                    {
                        rollback();
                        return lux::cxx::unexpected(
                            EScriptSystemError::CAPACITY_EXCEEDED);
                    }
                    bucket->handlers.push_back({
                        instance,
                        method->call,
                        mount.id,
                        ordinal,
                        ecs::NullEntity});
                }
                else
                {
                    const auto event_target =
                        std::get<EventScriptTarget>(binding.target);
                    auto* bucket = findEventBucket(findEvent(event_target));
                    if (!bucket || !sameEventSignature(
                            *function,
                            bucket->endpoint->payload_type))
                    {
                        rollback();
                        return lux::cxx::unexpected(
                            EScriptSystemError::SIGNATURE_MISMATCH);
                    }
                    ecs::Entity entity{ecs::NullEntity};
                    if (bucket->endpoint->route == EEventRoute::ENTITY_TARGETED)
                    {
                        const auto* scope =
                            std::get_if<EntityScriptScope>(&instance->scope);
                        if (!scope)
                        {
                            rollback();
                            return lux::cxx::unexpected(
                                EScriptSystemError::SCOPE_MISMATCH);
                        }
                        entity = scope->self;
                    }
                    if (bucket->handlers.size() >= capacities.handlers)
                    {
                        rollback();
                        return lux::cxx::unexpected(
                            EScriptSystemError::CAPACITY_EXCEEDED);
                    }
                    bucket->handlers.push_back({
                        instance,
                        method->call,
                        mount.id,
                        ordinal,
                        entity});
                }
            }
            if (instance->backend->startInstance &&
                instance->backend->startInstance(
                    instance->backend->context,
                    instance->backend_instance) != EScriptBackendResult::SUCCESS)
            {
                rollback();
                return lux::cxx::unexpected(EScriptSystemError::BACKEND_FAILURE);
            }
            instance->state = EInstanceState::ACTIVE;
            return {};
        }

        void removeHandlers(Instance& instance)
        {
            for (auto& bucket : hooks)
            {
                bucket.handlers.erase(
                    std::remove_if(
                        bucket.handlers.begin(),
                        bucket.handlers.end(),
                        [&](const Handler& handler) noexcept
                        {
                            return handler.instance == std::addressof(instance);
                        }
                    ),
                    bucket.handlers.end()
                );
            }
            for (auto& bucket : events)
            {
                bucket.handlers.erase(
                    std::remove_if(
                        bucket.handlers.begin(),
                        bucket.handlers.end(),
                        [&](const Handler& handler) noexcept
                        {
                            return handler.instance == std::addressof(instance);
                        }
                    ),
                    bucket.handlers.end()
                );
                rebuildRanges(bucket);
            }
        }

        void queue(ecs::Entity entity) noexcept
        {
            if (dirty_current.size() >= capacities.mutations)
            {
                full_resync = true;
                return;
            }
            if (std::find(
                    dirty_current.begin(),
                    dirty_current.end(),
                    entity) == dirty_current.end())
            {
                dirty_current.push_back(entity);
            }
        }

        void onAttachmentConstructed(
            ecs::Registry&,
            ecs::Entity entity
        ) noexcept
        {
            queue(entity);
        }

        void onAttachmentUpdated(
            ecs::Registry&,
            ecs::Entity entity
        ) noexcept
        {
            queue(entity);
        }

        void onAttachmentDestroyed(
            ecs::Registry&,
            ecs::Entity entity
        ) noexcept
        {
            queue(entity);
        }

        void disconnectEndpoints() noexcept
        {
            for (auto& bucket : hooks)
            {
                if (bucket.token.valid())
                {
                    bucket.endpoint->disconnect(
                        bucket.endpoint->context,
                        bucket.token
                    );
                    bucket.endpoint->flush(bucket.endpoint->context);
                    bucket.token = {};
                }
            }
            for (auto& bucket : events)
            {
                if (bucket.token.valid())
                {
                    bucket.endpoint->disconnect(
                        bucket.endpoint->context,
                        bucket.token
                    );
                    bucket.endpoint->flush(bucket.endpoint->context);
                    bucket.token = {};
                }
            }
        }
    };

    lux::cxx::expected<ScriptSystem, EScriptSystemError> ScriptSystem::create(
        const SimulationDescription& simulation,
        const ScriptSystemDescription& description,
        ecs::Registry& registry,
        ScriptSystemCapacities capacities,
        ResidentScriptResolver assets,
        ScriptWorldResolver world,
        std::span<const ScriptBackendDescriptor> backends,
        std::span<const ScriptHookEndpointDescriptor> hooks,
        std::span<const ScriptEventEndpointDescriptor> events,
        ScriptHostApi host
    ) noexcept
    {
        if (capacities.instances == 0U || capacities.prepared_methods == 0U ||
            capacities.handlers == 0U || capacities.failures == 0U ||
            capacities.mutations == 0U || !assets.resolve)
        {
            return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);
        }
        for (std::size_t index{}; index < backends.size(); ++index)
        {
            const auto& backend = backends[index];
            if (backend.kind == lux::rdesc::Script::Kind::UNKNOWN ||
                !backend.createInstance || !backend.prepareMethod ||
                !backend.releaseMethod || !backend.destroyInstance)
            {
                return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);
            }
            for (std::size_t previous{}; previous < index; ++previous)
            {
                if (backends[previous].kind == backend.kind)
                    return lux::cxx::unexpected(
                        EScriptSystemError::DUPLICATE_BACKEND_KIND);
            }
        }
        for (std::size_t index{}; index < hooks.size(); ++index)
        {
            const auto described = simulation.findHookPoint(
                hooks[index].system,
                hooks[index].hook
            );
            if (!hooks[index].system.valid() || !hooks[index].hook.valid() ||
                !hooks[index].connect || !hooks[index].disconnect ||
                !hooks[index].flush || !described ||
                described.parameterCount() !=
                    hooks[index].signature.parameters.size() ||
                !hooks[index].signature.returns.empty())
            {
                return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);
            }
            for (std::size_t parameter{};
                 parameter < described.parameterCount(); ++parameter)
            {
                if (described.parameterAt(parameter) !=
                    hooks[index].signature.parameters[parameter])
                {
                    return lux::cxx::unexpected(
                        EScriptSystemError::SIGNATURE_MISMATCH);
                }
            }
            for (std::size_t previous{}; previous < index; ++previous)
            {
                if (hooks[previous].system == hooks[index].system &&
                    hooks[previous].hook == hooks[index].hook)
                {
                    return lux::cxx::unexpected(
                        EScriptSystemError::DUPLICATE_ENDPOINT);
                }
            }
        }
        for (std::size_t index{}; index < events.size(); ++index)
        {
            const auto described = simulation.findEvent(
                events[index].system,
                events[index].event
            );
            if (!events[index].system.valid() || !events[index].event.valid() ||
                !events[index].connect || !events[index].disconnect ||
                !events[index].flush || !described ||
                described.route() != events[index].route ||
                described.payloadType() != events[index].payload_type.type_id ||
                described.payloadSchemaName() !=
                    events[index].payload_type.canonical_name ||
                events[index].payload_type.pass !=
                    lux::semantic::EValuePass::CONST_REF)
            {
                return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);
            }
            for (std::size_t previous{}; previous < index; ++previous)
            {
                if (events[previous].system == events[index].system &&
                    events[previous].event == events[index].event)
                {
                    return lux::cxx::unexpected(
                        EScriptSystemError::DUPLICATE_ENDPOINT);
                }
            }
        }
        try
        {
            auto state = std::make_unique<State>();
            state->simulation = &simulation;
            state->description = &description;
            state->registry = &registry;
            state->capacities = capacities;
            state->assets = assets;
            state->world = world;
            state->host = host;
            state->backends.assign(backends.begin(), backends.end());
            state->hook_endpoints.assign(hooks.begin(), hooks.end());
            state->event_endpoints.assign(events.begin(), events.end());
            state->instances.reserve(capacities.instances);
            state->hooks.reserve(capacities.hook_buckets);
            state->events.reserve(capacities.event_buckets);
            state->failures.reserve(capacities.failures);
            state->dirty_current.reserve(capacities.mutations);
            state->dirty_processing.reserve(capacities.mutations);
            return ScriptSystem(std::move(state));
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(EScriptSystemError::ALLOCATION_FAILURE);
        }
    }

    ScriptSystem::ScriptSystem(std::unique_ptr<State> state) noexcept
        : state_(std::move(state))
    {
    }

    ScriptSystem::ScriptSystem(ScriptSystem&&) noexcept = default;
    ScriptSystem& ScriptSystem::operator=(ScriptSystem&&) noexcept = default;

    ScriptSystem::~ScriptSystem() noexcept
    {
        if (state_ && !state_->shut_down)
            static_cast<void>(shutdown());
    }

    lux::cxx::expected<void, EScriptSystemError> ScriptSystem::prepare() noexcept
    {
        if (!state_ || state_->shut_down)
            return lux::cxx::unexpected(EScriptSystemError::SHUT_DOWN);
        if (state_->prepared)
            return {};
        try
        {
            for (const auto& mount : state_->description->mounts())
            {
                const auto buckets = state_->ensureBuckets(mount);
                if (!buckets)
                    return lux::cxx::unexpected(buckets.error());
            }
            state_->constructed = state_->registry
                ->on_construct<detail::ScriptAttachment>()
                .template connect<&State::onAttachmentConstructed>(*state_);
            state_->updated = state_->registry
                ->on_update<detail::ScriptAttachment>()
                .template connect<&State::onAttachmentUpdated>(*state_);
            state_->destroyed = state_->registry
                ->on_destroy<detail::ScriptAttachment>()
                .template connect<&State::onAttachmentDestroyed>(*state_);

            for (const auto& mount : state_->description->mounts())
            {
                const auto attached = state_->attachMount(mount);
                if (!attached)
                {
                    static_cast<void>(shutdown());
                    return lux::cxx::unexpected(attached.error());
                }
            }
            state_->sortHandlers();
            for (auto& bucket : state_->hooks)
            {
                const auto connected = bucket.endpoint->connect(
                    bucket.endpoint->context,
                    &bucket,
                    &State::invokeHookLane
                );
                if (!connected)
                {
                    static_cast<void>(shutdown());
                    return lux::cxx::unexpected(
                        EScriptSystemError::ENDPOINT_CONNECTION_FAILURE);
                }
                bucket.token = connected.token;
                if (bucket.endpoint->flush(bucket.endpoint->context) !=
                    EEndpointMutationError::NONE)
                {
                    static_cast<void>(shutdown());
                    return lux::cxx::unexpected(
                        EScriptSystemError::ENDPOINT_CONNECTION_FAILURE);
                }
            }
            for (auto& bucket : state_->events)
            {
                const auto connected = bucket.endpoint->connect(
                    bucket.endpoint->context,
                    &bucket,
                    &State::dispatchEvent
                );
                if (!connected)
                {
                    static_cast<void>(shutdown());
                    return lux::cxx::unexpected(
                        EScriptSystemError::ENDPOINT_CONNECTION_FAILURE);
                }
                bucket.token = connected.token;
                if (bucket.endpoint->flush(bucket.endpoint->context) !=
                    EEndpointMutationError::NONE)
                {
                    static_cast<void>(shutdown());
                    return lux::cxx::unexpected(
                        EScriptSystemError::ENDPOINT_CONNECTION_FAILURE);
                }
            }
            state_->dirty_current.clear();
            state_->dirty_processing.clear();
            state_->full_resync = false;
            state_->prepared = true;
            return {};
        }
        catch (const std::bad_alloc&)
        {
            static_cast<void>(shutdown());
            return lux::cxx::unexpected(EScriptSystemError::ALLOCATION_FAILURE);
        }
    }

    lux::cxx::expected<void, EScriptSystemError>
    ScriptSystem::flushMutations() noexcept
    {
        if (!state_ || state_->shut_down)
            return lux::cxx::unexpected(EScriptSystemError::SHUT_DOWN);
        if (!state_->prepared)
            return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);

        for (auto& instance : state_->instances)
        {
            if (instance.state != EInstanceState::RETIRING)
                continue;
            state_->removeHandlers(instance);
            state_->releaseInstance(instance, instance.stop_reason);
        }
        std::swap(state_->dirty_current, state_->dirty_processing);
        state_->dirty_current.clear();
        const bool full_resync = std::exchange(state_->full_resync, false);

        const auto reconcile = [&](ecs::Entity entity)
        {
            for (auto& instance : state_->instances)
            {
                const auto* scope = std::get_if<EntityScriptScope>(
                    &instance.scope);
                if (instance.state != EInstanceState::ACTIVE || !scope ||
                    scope->self != entity)
                {
                    continue;
                }
                const bool attachment_matches =
                    state_->registry->valid(entity) &&
                    state_->registry->all_of<detail::ScriptAttachment>(entity) &&
                    std::get<EntityScriptMount>(instance.authored->scope).object ==
                        state_->registry
                            ->get<detail::ScriptAttachment>(entity).object;
                if (!attachment_matches)
                {
                    instance.state = EInstanceState::RETIRING;
                    instance.stop_reason = EBehaviorStopReason::ENTITY_DESTROYED;
                    state_->removeHandlers(instance);
                    state_->releaseInstance(instance, instance.stop_reason);
                }
            }
            if (!state_->registry->valid(entity) ||
                !state_->registry->all_of<detail::ScriptAttachment>(entity))
            {
                return lux::cxx::expected<void, EScriptSystemError>{};
            }
            const auto object = state_->registry
                ->get<detail::ScriptAttachment>(entity).object;
            const auto mount = std::find_if(
                state_->description->mounts().begin(),
                state_->description->mounts().end(),
                [&](const auto& candidate) noexcept
                {
                    const auto* scope =
                        std::get_if<EntityScriptMount>(&candidate.scope);
                    return scope && scope->object == object;
                }
            );
            if (mount != state_->description->mounts().end())
            {
                const auto attached = state_->attachMount(*mount, entity);
                if (!attached)
                    return attached;
            }
            return lux::cxx::expected<void, EScriptSystemError>{};
        };

        if (full_resync)
        {
            for (auto& instance : state_->instances)
            {
                const auto* scope = std::get_if<EntityScriptScope>(
                    &instance.scope);
                if (instance.state != EInstanceState::ACTIVE || !scope)
                    continue;
                const auto reconciled = reconcile(scope->self);
                if (!reconciled)
                {
                    state_->full_resync = true;
                    state_->dirty_processing.clear();
                    return reconciled;
                }
            }
            const auto view = state_->registry
                ->view<detail::ScriptAttachment>();
            for (const auto entity : view)
            {
                const auto reconciled = reconcile(entity);
                if (!reconciled)
                {
                    state_->full_resync = true;
                    state_->dirty_processing.clear();
                    return reconciled;
                }
            }
        }
        else
        {
            for (const auto entity : state_->dirty_processing)
            {
                const auto reconciled = reconcile(entity);
                if (!reconciled)
                {
                    state_->queue(entity);
                    for (const auto pending : state_->dirty_processing)
                        state_->queue(pending);
                    state_->dirty_processing.clear();
                    return reconciled;
                }
            }
        }
        state_->dirty_processing.clear();
        state_->sortHandlers();
        return {};
    }

    lux::cxx::expected<void, EScriptSystemError> ScriptSystem::shutdown() noexcept
    {
        if (!state_ || state_->shut_down)
            return {};
        state_->constructed.release();
        state_->updated.release();
        state_->destroyed.release();
        state_->disconnectEndpoints();
        for (auto& instance : state_->instances)
        {
            if (instance.state != EInstanceState::DEAD)
            {
                state_->releaseInstance(
                    instance,
                    EBehaviorStopReason::SIMULATION_STOPPED
                );
            }
        }
        state_->hooks.clear();
        state_->events.clear();
        state_->dirty_current.clear();
        state_->dirty_processing.clear();
        state_->prepared = false;
        state_->shut_down = true;
        return {};
    }

    std::size_t ScriptSystem::activeInstanceCount() const noexcept
    {
        if (!state_)
            return 0U;
        return static_cast<std::size_t>(std::count_if(
            state_->instances.begin(),
            state_->instances.end(),
            [](const auto& instance) noexcept
            {
                return instance.state == EInstanceState::ACTIVE;
            }
        ));
    }

    std::span<const ScriptSystemFailure> ScriptSystem::failures() const noexcept
    {
        return state_ ? std::span<const ScriptSystemFailure>(state_->failures)
                      : std::span<const ScriptSystemFailure>{};
    }
}
