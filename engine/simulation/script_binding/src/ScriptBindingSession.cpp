#include <lux/engine/simulation/ScriptBindingSession.hpp>
#include <lux/engine/simulation/ScriptBindingCompatibility.hpp>

#include <entt/entity/entity.hpp>
#include <entt/signal/sigh.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <type_traits>
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

        [[nodiscard]] EScriptBindingError mapBackendResult(
            EScriptBackendResult result
        ) noexcept
        {
            switch (result)
            {
            case EScriptBackendResult::CAPACITY_EXCEEDED:
                return EScriptBindingError::CAPACITY_EXCEEDED;
            case EScriptBackendResult::ALLOCATION_FAILURE:
                return EScriptBindingError::ALLOCATION_FAILURE;
            case EScriptBackendResult::UNSUPPORTED_MARSHAL_TYPE:
                return EScriptBindingError::UNSUPPORTED_MARSHAL_TYPE;
            case EScriptBackendResult::UNSUPPORTED_MODEL:
            case EScriptBackendResult::UNSUPPORTED_SIGNATURE:
                return EScriptBindingError::SIGNATURE_MISMATCH;
            case EScriptBackendResult::CONSTRUCTION_FAILURE:
            case EScriptBackendResult::SUCCESS:
                return EScriptBindingError::BACKEND_CONSTRUCTION_FAILURE;
            }
            return EScriptBindingError::BACKEND_CONSTRUCTION_FAILURE;
        }
    }

    bool ScriptInstanceHostContext::attached() const noexcept
    {
        return api_ != nullptr && self_ != ecs::NullEntity;
    }

    ecs::Entity ScriptInstanceHostContext::self() const noexcept
    {
        return attached() ? self_ : ecs::NullEntity;
    }

    const void* ScriptInstanceHostContext::read(
        std::uint64_t component_type
    ) const noexcept
    {
        return attached() && api_->read
            ? api_->read(api_->context, self_, component_type)
            : nullptr;
    }

    bool ScriptInstanceHostContext::patch(
        std::uint64_t component_type,
        const void* value
    ) const noexcept
    {
        return attached() && api_->patch &&
            api_->patch(api_->context, self_, component_type, value);
    }

    bool ScriptInstanceHostContext::command(
        EScriptHostCommand command_value,
        std::uint64_t component_type,
        const void* value
    ) const noexcept
    {
        return attached() && api_->command &&
            api_->command(
                api_->context,
                command_value,
                self_,
                component_type,
                value
            );
    }

    void ScriptInstanceHostContext::attach(
        const ScriptHostApi& api,
        ecs::Entity entity
    ) noexcept
    {
        api_ = std::addressof(api);
        self_ = entity;
    }

    struct ScriptBindingSession::State final
    {
        enum class EMountState : std::uint8_t
        {
            CONSTRUCTING,
            ACTIVE,
            RETIRING,
            DEAD,
        };

        struct PreparedMethod final
        {
            lux::script::ScriptSymbolId symbol{};
            lux::script::BoundScriptCall call;
        };

        struct MountRuntime final
        {
            ScriptMountDescription authored;
            ecs::Entity self{ecs::NullEntity};
            std::size_t mount_order{};
            ScriptInstanceHostContext host;
            const lux::asset::ScriptAssetContent* asset{};
            void* lease{};
            void (*release_lease)(void*) noexcept{};
            std::size_t backend{};
            ScriptBackendInstance instance;
            std::vector<PreparedMethod> methods;
            EMountState state{EMountState::CONSTRUCTING};
            EBehaviorStopReason stop_reason{EBehaviorStopReason::MOUNT_REMOVED};
            bool lifecycle_started{};
            bool stop_called{};
        };

        struct Handler final
        {
            MountRuntime* mount{};
            PreparedMethod* method{};
        };

        struct Hook final
        {
            std::size_t system{};
            std::size_t member{};
            ESystemHookCardinality cardinality{ESystemHookCardinality::MULTI};
            std::vector<Handler> global_handlers;
        };

        struct Event final
        {
            std::size_t system{};
            std::size_t member{};
            ScriptHookSlot dispatch_hook;
            ESystemEventTarget target{ESystemEventTarget::GLOBAL};
            std::vector<Handler> global_handlers;
        };

        struct EntitySidecar final
        {
            ecs::Entity owner{ecs::NullEntity};
            std::vector<std::vector<Handler>> hook_handlers;
            std::vector<std::vector<Handler>> event_handlers;
        };

        struct DirtyEntity final
        {
            ecs::Entity entity{ecs::NullEntity};
            EBehaviorStopReason reason{EBehaviorStopReason::MOUNT_REMOVED};
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
              constructed(source_registry.on_construct<ScriptComponent>()
                  .connect<&State::onConstruct>(*this)),
              updated(source_registry.on_update<ScriptComponent>()
                  .connect<&State::onUpdate>(*this)),
              destroyed(source_registry.on_destroy<ScriptComponent>()
                  .connect<&State::onDestroy>(*this))
        {
            backends.assign(source_backends.begin(), source_backends.end());
            mounts.reserve(capacities.mount_instances);
            dirty.reserve(capacities.dirty_entities);
            failures.reserve(capacities.failures);
            sidecars.reserve(capacities.entity_slots);
            entity_to_sidecar.assign(capacities.entity_slots, kInvalidIndex);
            makeSlots();
        }

        ~State()
        {
            if (!shut_down)
                (void)shutdown();
        }

        void makeSlots()
        {
            for (std::size_t system_index{};
                 system_index < description.systemCount(); ++system_index)
            {
                const auto system = description.systemAt(system_index);
                for (std::size_t hook_index{};
                     hook_index < system.hookPointCount(); ++hook_index)
                {
                    hooks.push_back(Hook{
                        system_index,
                        hook_index,
                        system.hookPointAt(hook_index).cardinality(),
                        {}});
                }
            }
            for (std::size_t system_index{};
                 system_index < description.systemCount(); ++system_index)
            {
                const auto system = description.systemAt(system_index);
                for (std::size_t event_index{};
                     event_index < system.eventCount(); ++event_index)
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
                if (backends[index].kind == kind)
                    return std::addressof(backends[index]);
            }
            return nullptr;
        }

        [[nodiscard]] const lux::rdesc::ScriptFunction* findFunction(
            const lux::asset::ScriptAssetContent& asset,
            lux::script::ScriptSymbolId symbol
        ) const noexcept
        {
            const auto found = std::find_if(
                asset.description.exports.begin(),
                asset.description.exports.end(),
                [symbol](const auto& function) noexcept
                {
                    return function.symbol_id == symbol;
                }
            );
            return found == asset.description.exports.end()
                ? nullptr
                : std::addressof(*found);
        }

        [[nodiscard]] lux::cxx::expected<
            SimulationSystemView,
            EScriptBindingError> resolveSystem(
                const SystemTypeId& type,
                std::string_view instance
            ) noexcept
        {
            ++instrumentation.target_resolutions;
            if (!instance.empty())
            {
                const auto system = description.findSystem(instance);
                if (!system)
                    return lux::cxx::unexpected(
                        EScriptBindingError::TARGET_SYSTEM_NOT_FOUND);
                if (system.type() != type)
                    return lux::cxx::unexpected(
                        EScriptBindingError::TARGET_TYPE_MISMATCH);
                return system;
            }
            SimulationSystemView resolved;
            for (std::size_t index{}; index < description.systemCount(); ++index)
            {
                const auto candidate = description.systemAt(index);
                if (candidate.type() != type)
                    continue;
                if (resolved)
                    return lux::cxx::unexpected(
                        EScriptBindingError::TARGET_SYSTEM_AMBIGUOUS);
                resolved = candidate;
            }
            if (!resolved)
                return lux::cxx::unexpected(
                    EScriptBindingError::TARGET_SYSTEM_NOT_FOUND);
            return resolved;
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptBindingError>
        validateBinding(
            bool entity_scope,
            const lux::rdesc::ScriptFunction& function,
            const ScriptBindingDescription& binding
        ) noexcept
        {
            const auto compatibility = evaluateScriptBindingCompatibility(
                description,
                entity_scope
                    ? lux::rdesc::EScriptModel::ENTITY_BEHAVIOR
                    : lux::rdesc::EScriptModel::GLOBAL_MODULE,
                function,
                binding.target
            );
            switch (compatibility)
            {
            case EScriptBindingCompatibility::COMPATIBLE:
                return {};
            case EScriptBindingCompatibility::TARGET_NOT_FOUND:
                return lux::cxx::unexpected(
                    EScriptBindingError::MEMBER_NOT_FOUND);
            case EScriptBindingCompatibility::TARGET_AMBIGUOUS:
                return lux::cxx::unexpected(
                    EScriptBindingError::TARGET_SYSTEM_AMBIGUOUS);
            case EScriptBindingCompatibility::TARGET_TYPE_MISMATCH:
                return lux::cxx::unexpected(
                    EScriptBindingError::TARGET_TYPE_MISMATCH);
            case EScriptBindingCompatibility::SCOPE_MISMATCH:
                return lux::cxx::unexpected(
                    EScriptBindingError::SCOPE_MISMATCH);
            case EScriptBindingCompatibility::CARDINALITY_MISMATCH:
                return lux::cxx::unexpected(
                    EScriptBindingError::CARDINALITY_MISMATCH);
            case EScriptBindingCompatibility::INVALID_FUNCTION:
            case EScriptBindingCompatibility::SIGNATURE_MISMATCH:
                return lux::cxx::unexpected(
                    EScriptBindingError::SIGNATURE_MISMATCH);
            }
            return lux::cxx::unexpected(
                EScriptBindingError::SIGNATURE_MISMATCH);
        }

        [[nodiscard]] PreparedMethod* findMethod(
            MountRuntime& mount,
            lux::script::ScriptSymbolId symbol
        ) noexcept
        {
            const auto found = std::find_if(
                mount.methods.begin(),
                mount.methods.end(),
                [symbol](const auto& method) noexcept
                {
                    return method.symbol == symbol;
                }
            );
            return found == mount.methods.end() ? nullptr : std::addressof(*found);
        }

        void recordFailure(
            MountRuntime& mount,
            lux::script::ScriptSymbolId symbol,
            std::int32_t status
        ) noexcept
        {
            mount.state = EMountState::RETIRING;
            mount.stop_reason = EBehaviorStopReason::MOUNT_REMOVED;
            if (failures.size() < capacities.failures)
            {
                failures.push_back(ScriptBindingFailure{
                    EScriptBindingError::INVOCATION_FAILURE,
                    mount.authored.id,
                    symbol,
                    mount.self,
                    status});
            }
        }

        [[nodiscard]] ScriptDispatchResult invoke(
            Handler handler,
            lux_script_call_frame& frame,
            bool single
        ) noexcept
        {
            ScriptDispatchResult result;
            if (!handler.mount || !handler.method ||
                handler.mount->state != EMountState::ACTIVE ||
                !handler.method->call)
            {
                return result;
            }
            frame.world_context = std::addressof(handler.mount->host);
            frame.user_context = handler.method->call.context;
            const auto status = handler.method->call.invoke(
                std::addressof(frame));
            result.calls = 1U;
            result.status = status;
            if (status != 0)
            {
                result.failures = 1U;
                recordFailure(
                    *handler.mount,
                    handler.method->symbol,
                    status
                );
            }
            (void)single;
            return result;
        }

        [[nodiscard]] bool invokeLifecycle(
            MountRuntime& mount,
            EBehaviorLifecyclePoint point,
            EBehaviorStopReason reason = EBehaviorStopReason::MOUNT_REMOVED
        ) noexcept
        {
            for (const auto& binding : mount.authored.bindings)
            {
                const auto* lifecycle =
                    std::get_if<BehaviorLifecycleBindingTarget>(
                        std::addressof(binding.target));
                if (!lifecycle || lifecycle->point != point)
                    continue;
                auto* method = findMethod(mount, binding.function);
                if (!method || !method->call)
                    continue;
                std::uint32_t reason_value = static_cast<std::uint32_t>(reason);
                lux_script_value_slot reason_slot{
                    LUX_SCRIPT_VK_UINT32,
                    {},
                    sizeof(reason_value),
                    lux::script::scriptSemanticTypeId(
                        BehaviorStopReasonCanonicalName),
                    std::addressof(reason_value)};
                lux_script_call_frame frame{
                    point == EBehaviorLifecyclePoint::STOP
                        ? std::addressof(reason_slot)
                        : nullptr,
                    point == EBehaviorLifecyclePoint::STOP ? 1U : 0U,
                    0U,
                    nullptr,
                    0U,
                    0U,
                    std::addressof(mount.host),
                    method->call.context};
                ++instrumentation.frame_builds;
                const auto status = method->call.invoke(std::addressof(frame));
                if (status != 0)
                {
                    recordFailure(mount, method->symbol, status);
                    return false;
                }
            }
            return true;
        }

        void releaseMount(MountRuntime& mount, bool invoke_stop) noexcept
        {
            if (invoke_stop && mount.lifecycle_started && !mount.stop_called)
            {
                mount.stop_called = true;
                (void)invokeLifecycle(
                    mount,
                    EBehaviorLifecyclePoint::STOP,
                    mount.stop_reason
                );
            }
            if (mount.backend < backends.size())
            {
                const auto& backend = backends[mount.backend];
                for (auto& method : mount.methods)
                {
                    if (backend.releaseMethod && method.call)
                    {
                        backend.releaseMethod(
                            backend.context,
                            mount.instance,
                            method.call
                        );
                    }
                }
                mount.methods.clear();
                if (backend.destroyInstance && mount.instance)
                {
                    backend.destroyInstance(backend.context, mount.instance);
                }
            }
            mount.instance = {};
            if (mount.release_lease)
                mount.release_lease(mount.lease);
            mount.lease = nullptr;
            mount.release_lease = nullptr;
            mount.asset = nullptr;
            mount.state = EMountState::DEAD;
        }

        [[nodiscard]] lux::cxx::expected<
            std::unique_ptr<MountRuntime>,
            EScriptBindingError> createMount(
                const ScriptMountDescription& authored,
                ecs::Entity self,
                std::size_t mount_order
            ) noexcept
        {
            if (mounts.size() >= capacities.mount_instances)
                return lux::cxx::unexpected(
                    EScriptBindingError::CAPACITY_EXCEEDED);
            ResolvedScriptAsset resolved;
            ++instrumentation.asset_resolutions;
            if (!resolver.resolve ||
                !resolver.resolve(resolver.context, authored.script, resolved) ||
                !resolved.asset)
            {
                return lux::cxx::unexpected(
                    EScriptBindingError::ASSET_NOT_FOUND);
            }
            const auto release_resolved = [&]() noexcept
            {
                if (resolved.release)
                    resolved.release(resolved.lease);
            };
            if (!lux::rdesc::validScriptDescription(
                    resolved.asset->description))
            {
                release_resolved();
                return lux::cxx::unexpected(
                    EScriptBindingError::INVALID_ASSET);
            }
            const bool entity_scope = self != ecs::NullEntity;
            if (entity_scope !=
                (resolved.asset->description.model ==
                 lux::rdesc::EScriptModel::ENTITY_BEHAVIOR))
            {
                release_resolved();
                return lux::cxx::unexpected(
                    EScriptBindingError::SCOPE_MISMATCH);
            }
            for (const auto& binding : authored.bindings)
            {
                const auto* function = findFunction(*resolved.asset, binding.function);
                if (!function)
                {
                    release_resolved();
                    return lux::cxx::unexpected(
                        EScriptBindingError::SYMBOL_NOT_FOUND);
                }
                const auto valid = validateBinding(
                    entity_scope,
                    *function,
                    binding
                );
                if (!valid)
                {
                    release_resolved();
                    return lux::cxx::unexpected(valid.error());
                }
            }

            std::size_t backend_index{};
            const auto* backend = backendFor(
                resolved.asset->description.kind(),
                backend_index
            );
            if (!backend)
            {
                release_resolved();
                return lux::cxx::unexpected(
                    EScriptBindingError::BACKEND_NOT_AVAILABLE);
            }
            std::unique_ptr<MountRuntime> runtime;
            try
            {
                runtime = std::make_unique<MountRuntime>();
                runtime->authored = authored;
                runtime->self = self;
                runtime->mount_order = mount_order;
                runtime->asset = resolved.asset;
                runtime->lease = resolved.lease;
                runtime->release_lease = resolved.release;
                resolved.lease = nullptr;
                resolved.release = nullptr;
                runtime->backend = backend_index;
                runtime->methods.reserve(authored.bindings.size());
                ScriptInstanceCreateContext create_context{
                    authored.script,
                    authored.id,
                    self,
                    std::addressof(runtime->host)};
                ++instrumentation.instance_creates;
                const auto create_result = backend->createInstance(
                    backend->context,
                    create_context,
                    *resolved.asset,
                    runtime->instance
                );
                if (create_result != EScriptBackendResult::SUCCESS)
                {
                    releaseMount(*runtime, false);
                    return lux::cxx::unexpected(mapBackendResult(create_result));
                }
                runtime->host.attach(host_api, self);
                for (const auto& binding : authored.bindings)
                {
                    if (findMethod(*runtime, binding.function))
                        continue;
                    if (preparedMethodCount() >= capacities.prepared_methods)
                    {
                        releaseMount(*runtime, false);
                        return lux::cxx::unexpected(
                            EScriptBindingError::CAPACITY_EXCEEDED);
                    }
                    const auto* function = findFunction(
                        *resolved.asset,
                        binding.function
                    );
                    lux::script::BoundScriptCall call;
                    ++instrumentation.method_prepares;
                    const auto prepare_result = backend->prepareMethod(
                        backend->context,
                        runtime->instance,
                        *function,
                        call
                    );
                    if (prepare_result != EScriptBackendResult::SUCCESS || !call)
                    {
                        releaseMount(*runtime, false);
                        return lux::cxx::unexpected(
                            mapBackendResult(prepare_result));
                    }
                    runtime->methods.push_back(
                        PreparedMethod{binding.function, call});
                }
                runtime->lifecycle_started = true;
                if (!invokeLifecycle(
                        *runtime,
                        EBehaviorLifecyclePoint::CONSTRUCT) ||
                    !invokeLifecycle(*runtime, EBehaviorLifecyclePoint::START))
                {
                    releaseMount(*runtime, true);
                    return lux::cxx::unexpected(
                        EScriptBindingError::INVOCATION_FAILURE);
                }
                runtime->state = EMountState::ACTIVE;
                return runtime;
            }
            catch (const std::bad_alloc&)
            {
                if (runtime)
                    releaseMount(*runtime, false);
                release_resolved();
                return lux::cxx::unexpected(
                    EScriptBindingError::ALLOCATION_FAILURE);
            }
        }

        [[nodiscard]] std::size_t preparedMethodCount() const noexcept
        {
            std::size_t count{};
            for (const auto& mount : mounts)
                count += mount->methods.size();
            return count;
        }

        [[nodiscard]] MountRuntime* findMount(
            ecs::Entity self,
            ScriptMountId id
        ) const noexcept
        {
            for (const auto& mount : mounts)
            {
                if (mount->state != EMountState::DEAD &&
                    mount->self == self && mount->authored.id == id)
                {
                    return mount.get();
                }
            }
            return nullptr;
        }

        void markRemoved(
            ecs::Entity self,
            const ScriptComponent* desired,
            EBehaviorStopReason reason
        ) noexcept
        {
            for (auto& runtime : mounts)
            {
                if (runtime->self != self ||
                    runtime->state == EMountState::DEAD)
                    continue;
                const auto found = desired
                    ? std::find_if(
                        desired->mounts.begin(),
                        desired->mounts.end(),
                        [&](const auto& mount) noexcept
                        {
                            return mount.id == runtime->authored.id &&
                                mount.script == runtime->authored.script;
                        })
                    : std::vector<ScriptMountDescription>::const_iterator{};
                if (!desired || found == desired->mounts.end())
                {
                    runtime->state = EMountState::RETIRING;
                    runtime->stop_reason = reason;
                }
            }
        }

        void destroyRetired() noexcept
        {
            for (auto& mount : mounts)
            {
                if (mount->state == EMountState::RETIRING)
                    releaseMount(*mount, true);
            }
            mounts.erase(
                std::remove_if(
                    mounts.begin(),
                    mounts.end(),
                    [](const auto& mount) noexcept
                    {
                        return mount->state == EMountState::DEAD;
                    }
                ),
                mounts.end()
            );
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptBindingError>
        reconcileMount(
            MountRuntime& runtime,
            const ScriptMountDescription& authored,
            std::size_t mount_order
        ) noexcept
        {
            const bool entity_scope = runtime.self != ecs::NullEntity;
            for (const auto& binding : authored.bindings)
            {
                const auto* function = findFunction(*runtime.asset, binding.function);
                if (!function)
                    return lux::cxx::unexpected(
                        EScriptBindingError::SYMBOL_NOT_FOUND);
                const auto valid = validateBinding(
                    entity_scope,
                    *function,
                    binding
                );
                if (!valid)
                    return lux::cxx::unexpected(valid.error());
            }
            const auto& backend = backends[runtime.backend];
            for (auto iterator = runtime.methods.begin();
                 iterator != runtime.methods.end();)
            {
                const bool still_used = std::any_of(
                    authored.bindings.begin(),
                    authored.bindings.end(),
                    [&](const auto& binding) noexcept
                    {
                        return binding.function == iterator->symbol;
                    }
                );
                if (still_used)
                {
                    ++iterator;
                    continue;
                }
                if (backend.releaseMethod && iterator->call)
                {
                    backend.releaseMethod(
                        backend.context,
                        runtime.instance,
                        iterator->call
                    );
                }
                iterator = runtime.methods.erase(iterator);
            }
            for (const auto& binding : authored.bindings)
            {
                if (findMethod(runtime, binding.function))
                    continue;
                if (preparedMethodCount() >= capacities.prepared_methods)
                    return lux::cxx::unexpected(
                        EScriptBindingError::CAPACITY_EXCEEDED);
                const auto* function = findFunction(*runtime.asset, binding.function);
                lux::script::BoundScriptCall call;
                ++instrumentation.method_prepares;
                const auto result = backend.prepareMethod(
                    backend.context,
                    runtime.instance,
                    *function,
                    call
                );
                if (result != EScriptBackendResult::SUCCESS || !call)
                    return lux::cxx::unexpected(mapBackendResult(result));
                runtime.methods.push_back(
                    PreparedMethod{binding.function, call});
            }
            runtime.authored = authored;
            runtime.mount_order = mount_order;
            return {};
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptBindingError>
        upsertOwner(
            ecs::Entity self,
            const std::vector<ScriptMountDescription>& authored_mounts
        ) noexcept
        {
            if (!validScriptMountList(authored_mounts))
                return lux::cxx::unexpected(
                    EScriptBindingError::INVALID_ASSET);
            for (std::size_t index{}; index < authored_mounts.size(); ++index)
            {
                const auto& authored = authored_mounts[index];
                if (auto* runtime = findMount(self, authored.id))
                {
                    const auto reconciled = reconcileMount(
                        *runtime,
                        authored,
                        index
                    );
                    if (!reconciled)
                        return reconciled;
                    continue;
                }
                auto created = createMount(authored, self, index);
                if (!created)
                    return lux::cxx::unexpected(created.error());
                mounts.push_back(std::move(*created));
            }
            return {};
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptBindingError>
        buildDispatch() noexcept
        {
            try
            {
                for (auto& hook : hooks)
                    hook.global_handlers.clear();
                for (auto& event : events)
                    event.global_handlers.clear();
                std::fill(
                    entity_to_sidecar.begin(),
                    entity_to_sidecar.end(),
                    kInvalidIndex
                );
                sidecars.clear();

                std::vector<MountRuntime*> ordered;
                ordered.reserve(mounts.size());
                for (auto& mount : mounts)
                {
                    if (mount->state == EMountState::ACTIVE)
                        ordered.push_back(mount.get());
                }
                std::sort(
                    ordered.begin(),
                    ordered.end(),
                    [](const auto* left, const auto* right) noexcept
                    {
                        if ((left->self == ecs::NullEntity) !=
                            (right->self == ecs::NullEntity))
                        {
                            return left->self == ecs::NullEntity;
                        }
                        if (left->self != right->self)
                            return ecs::entityBits(left->self) <
                                ecs::entityBits(right->self);
                        return left->mount_order < right->mount_order;
                    }
                );

                const auto sidecarFor = [&](ecs::Entity entity)
                    -> EntitySidecar*
                {
                    const auto index = entityIndex(entity);
                    if (index >= entity_to_sidecar.size())
                        return nullptr;
                    auto sidecar_index = entity_to_sidecar[index];
                    if (sidecar_index != kInvalidIndex)
                    {
                        auto& existing = sidecars[sidecar_index];
                        return existing.owner == entity
                            ? std::addressof(existing)
                            : nullptr;
                    }
                    if (sidecars.size() >= capacities.entity_slots)
                        return nullptr;
                    sidecar_index = static_cast<std::uint32_t>(sidecars.size());
                    sidecars.push_back(EntitySidecar{
                        entity,
                        std::vector<std::vector<Handler>>(hooks.size()),
                        std::vector<std::vector<Handler>>(events.size())});
                    entity_to_sidecar[index] = sidecar_index;
                    return std::addressof(sidecars.back());
                };

                for (auto* mount : ordered)
                {
                    EntitySidecar* sidecar{};
                    if (mount->self != ecs::NullEntity)
                    {
                        sidecar = sidecarFor(mount->self);
                        if (!sidecar)
                            return lux::cxx::unexpected(
                                EScriptBindingError::CAPACITY_EXCEEDED);
                    }
                    for (const auto& binding : mount->authored.bindings)
                    {
                        auto* method = findMethod(*mount, binding.function);
                        if (!method)
                            return lux::cxx::unexpected(
                                EScriptBindingError::SYMBOL_NOT_FOUND);
                        const Handler handler{mount, method};
                        const auto indexed = std::visit(
                            [&](const auto& target) noexcept -> bool
                            {
                                using Target =
                                    std::remove_cvref_t<decltype(target)>;
                                if constexpr (
                                    std::is_same_v<Target,
                                        SystemHookBindingTarget>)
                                {
                                    auto system = resolveSystem(
                                        target.system_type,
                                        target.system_instance
                                    );
                                    if (!system)
                                        return false;
                                    ScriptHookSlot resolved_slot;
                                    for (std::size_t index{};
                                         index < description.systemCount(); ++index)
                                    {
                                        if (description.systemAt(index).instanceName() ==
                                            system->instanceName())
                                        {
                                            resolved_slot = findHookSlot(
                                                index,
                                                target.hook
                                            );
                                            break;
                                        }
                                    }
                                    if (!resolved_slot)
                                        return false;
                                    if (sidecar)
                                        sidecar->hook_handlers[resolved_slot.value]
                                            .push_back(handler);
                                    else
                                        hooks[resolved_slot.value]
                                            .global_handlers.push_back(handler);
                                }
                                else if constexpr (
                                    std::is_same_v<Target,
                                        SystemEventBindingTarget>)
                                {
                                    auto system = resolveSystem(
                                        target.system_type,
                                        target.system_instance
                                    );
                                    if (!system)
                                        return false;
                                    ScriptEventSlot resolved_slot;
                                    for (std::size_t index{};
                                         index < description.systemCount(); ++index)
                                    {
                                        if (description.systemAt(index).instanceName() ==
                                            system->instanceName())
                                        {
                                            resolved_slot = findEventSlot(
                                                index,
                                                target.event
                                            );
                                            break;
                                        }
                                    }
                                    if (!resolved_slot)
                                        return false;
                                    if (sidecar)
                                        sidecar->event_handlers[resolved_slot.value]
                                            .push_back(handler);
                                    else
                                        events[resolved_slot.value]
                                            .global_handlers.push_back(handler);
                                }
                                return true;
                            },
                            binding.target
                        );
                        if (!indexed)
                            return lux::cxx::unexpected(
                                EScriptBindingError::MEMBER_NOT_FOUND);
                    }
                }
                return {};
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(
                    EScriptBindingError::ALLOCATION_FAILURE);
            }
        }

        void enqueue(
            ecs::Entity entity,
            EBehaviorStopReason reason
        ) noexcept
        {
            if (dirty.size() >= capacities.dirty_entities)
            {
                full_resync = true;
                return;
            }
            dirty.push_back(DirtyEntity{entity, reason});
        }

        void onConstruct(ecs::Registry&, ecs::Entity entity) noexcept
        {
            enqueue(entity, EBehaviorStopReason::MOUNT_REMOVED);
        }

        void onUpdate(ecs::Registry& source, ecs::Entity entity) noexcept
        {
            const auto* component = source.try_get<ScriptComponent>(entity);
            markRemoved(
                entity,
                component,
                EBehaviorStopReason::MOUNT_REMOVED
            );
            enqueue(entity, EBehaviorStopReason::MOUNT_REMOVED);
        }

        void onDestroy(ecs::Registry&, ecs::Entity entity) noexcept
        {
            markRemoved(entity, nullptr, EBehaviorStopReason::ENTITY_DESTROYED);
            enqueue(entity, EBehaviorStopReason::ENTITY_DESTROYED);
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptBindingError>
        prepareInitial() noexcept
        {
            if (shut_down)
                return lux::cxx::unexpected(
                    EScriptBindingError::SESSION_SHUT_DOWN);
            if (prepared_once)
                return {};
            try
            {
                std::vector<ScriptMountDescription> global_mounts;
                global_mounts.reserve(description.globalScriptMountCount());
                for (std::size_t index{};
                     index < description.globalScriptMountCount(); ++index)
                {
                    const auto view = description.globalScriptMountAt(index);
                    ScriptMountDescription mount{view.id(), view.script(), {}};
                    mount.bindings.reserve(view.bindingCount());
                    for (std::size_t binding{};
                         binding < view.bindingCount(); ++binding)
                    {
                        mount.bindings.push_back(*view.bindingAt(binding));
                    }
                    global_mounts.push_back(std::move(mount));
                }
                auto result = upsertOwner(ecs::NullEntity, global_mounts);
                if (!result)
                    return result;

                std::vector<ecs::Entity> entities;
                const auto view = registry->view<ScriptComponent>();
                for (const auto entity : view)
                    entities.push_back(entity);
                std::sort(
                    entities.begin(),
                    entities.end(),
                    [](auto left, auto right) noexcept
                    {
                        return ecs::entityBits(left) < ecs::entityBits(right);
                    }
                );
                for (const auto entity : entities)
                {
                    const auto& component = registry->get<ScriptComponent>(entity);
                    result = upsertOwner(entity, component.mounts);
                    if (!result)
                        return result;
                }
                result = buildDispatch();
                if (!result)
                    return result;
                dirty.clear();
                full_resync = false;
                prepared_once = true;
                return {};
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(
                    EScriptBindingError::ALLOCATION_FAILURE);
            }
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptBindingError>
        applyMutations() noexcept
        {
            if (shut_down)
                return lux::cxx::unexpected(
                    EScriptBindingError::SESSION_SHUT_DOWN);
            if (!prepared_once)
                return prepareInitial();
            try
            {
                if (full_resync)
                {
                    for (auto& runtime : mounts)
                    {
                        if (runtime->self == ecs::NullEntity ||
                            runtime->state == EMountState::DEAD)
                            continue;
                        const auto* component = registry->valid(runtime->self)
                            ? registry->try_get<ScriptComponent>(runtime->self)
                            : nullptr;
                        markRemoved(
                            runtime->self,
                            component,
                            registry->valid(runtime->self)
                                ? EBehaviorStopReason::MOUNT_REMOVED
                                : EBehaviorStopReason::ENTITY_DESTROYED
                        );
                    }
                    destroyRetired();
                    const auto view = registry->view<ScriptComponent>();
                    for (const auto entity : view)
                    {
                        const auto result = upsertOwner(
                            entity,
                            registry->get<ScriptComponent>(entity).mounts
                        );
                        if (!result)
                            return result;
                    }
                }
                else
                {
                    std::sort(
                        dirty.begin(),
                        dirty.end(),
                        [](const auto& left, const auto& right) noexcept
                        {
                            return ecs::entityBits(left.entity) <
                                ecs::entityBits(right.entity);
                        }
                    );
                    dirty.erase(
                        std::unique(
                            dirty.begin(),
                            dirty.end(),
                            [](const auto& left, const auto& right) noexcept
                            {
                                return left.entity == right.entity;
                            }
                        ),
                        dirty.end()
                    );
                    for (const auto change : dirty)
                    {
                        const auto* component = registry->valid(change.entity)
                            ? registry->try_get<ScriptComponent>(change.entity)
                            : nullptr;
                        markRemoved(change.entity, component, change.reason);
                    }
                    destroyRetired();
                    for (const auto change : dirty)
                    {
                        if (!registry->valid(change.entity))
                            continue;
                        const auto* component =
                            registry->try_get<ScriptComponent>(change.entity);
                        if (!component)
                            continue;
                        const auto result = upsertOwner(
                            change.entity,
                            component->mounts
                        );
                        if (!result)
                            return result;
                    }
                }
                dirty.clear();
                full_resync = false;
                destroyRetired();
                return buildDispatch();
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(
                    EScriptBindingError::ALLOCATION_FAILURE);
            }
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptBindingError>
        shutdown() noexcept
        {
            if (shut_down)
                return {};
            for (auto& mount : mounts)
            {
                if (mount->state != EMountState::DEAD)
                {
                    mount->state = EMountState::RETIRING;
                    mount->stop_reason =
                        EBehaviorStopReason::SIMULATION_STOPPED;
                }
            }
            destroyRetired();
            for (auto& hook : hooks)
                hook.global_handlers.clear();
            for (auto& event : events)
                event.global_handlers.clear();
            sidecars.clear();
            std::fill(
                entity_to_sidecar.begin(),
                entity_to_sidecar.end(),
                kInvalidIndex
            );
            shut_down = true;
            return {};
        }

        [[nodiscard]] EntitySidecar* sidecarFor(
            ecs::Entity entity
        ) noexcept
        {
            const auto index = entityIndex(entity);
            if (index >= entity_to_sidecar.size())
                return nullptr;
            const auto sidecar_index = entity_to_sidecar[index];
            if (sidecar_index == kInvalidIndex ||
                sidecar_index >= sidecars.size())
                return nullptr;
            auto& sidecar = sidecars[sidecar_index];
            return sidecar.owner == entity ? std::addressof(sidecar) : nullptr;
        }

        SimulationDescription description;
        ecs::Registry* registry{};
        ScriptBindingCapacities capacities;
        ScriptAssetResolver resolver;
        ScriptHostApi host_api;
        std::vector<ScriptBackendDescriptor> backends;
        std::vector<Hook> hooks;
        std::vector<Event> events;
        std::vector<std::unique_ptr<MountRuntime>> mounts;
        std::vector<EntitySidecar> sidecars;
        std::vector<std::uint32_t> entity_to_sidecar;
        std::vector<DirtyEntity> dirty;
        std::vector<ScriptBindingFailure> failures;
        ScriptBindingInstrumentation instrumentation;
        entt::scoped_connection constructed;
        entt::scoped_connection updated;
        entt::scoped_connection destroyed;
        bool full_resync{};
        bool prepared_once{};
        bool shut_down{};
    };

    ScriptBindingSession::ScriptBindingSession(
        std::unique_ptr<State> state
    ) noexcept
        : state_(std::move(state))
    {
    }

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
        if (!resolver.resolve || capacities.mount_instances == 0U ||
            capacities.prepared_methods == 0U ||
            capacities.entity_slots == 0U ||
            capacities.dirty_entities == 0U)
        {
            return lux::cxx::unexpected(
                EScriptBindingError::CAPACITY_EXCEEDED);
        }
        for (std::size_t index{}; index < backends.size(); ++index)
        {
            const auto& backend = backends[index];
            if (backend.kind == lux::rdesc::Script::Kind::UNKNOWN ||
                !backend.createInstance || !backend.prepareMethod ||
                !backend.destroyInstance)
            {
                return lux::cxx::unexpected(
                    EScriptBindingError::BACKEND_CONSTRUCTION_FAILURE);
            }
            for (std::size_t previous{}; previous < index; ++previous)
            {
                if (backends[previous].kind == backend.kind)
                {
                    return lux::cxx::unexpected(
                        EScriptBindingError::DUPLICATE_BACKEND_KIND);
                }
            }
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
                EScriptBindingError::ALLOCATION_FAILURE);
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
        return state_
            ? state_->prepareInitial()
            : lux::cxx::expected<void, EScriptBindingError>{
                lux::cxx::unexpected(EScriptBindingError::INVALID_SLOT)};
    }

    lux::cxx::expected<void, EScriptBindingError>
    ScriptBindingSession::applyQuiescentMutations() noexcept
    {
        return state_
            ? state_->applyMutations()
            : lux::cxx::expected<void, EScriptBindingError>{
                lux::cxx::unexpected(EScriptBindingError::INVALID_SLOT)};
    }

    lux::cxx::expected<void, EScriptBindingError>
    ScriptBindingSession::shutdown() noexcept
    {
        return state_
            ? state_->shutdown()
            : lux::cxx::expected<void, EScriptBindingError>{};
    }

    ScriptHookSlot ScriptBindingSession::hookSlot(
        std::string_view system_instance,
        std::string_view hook_name
    ) const noexcept
    {
        if (!state_)
            return {};
        for (std::size_t index{};
             index < state_->description.systemCount(); ++index)
        {
            if (state_->description.systemAt(index).instanceName() ==
                system_instance)
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
        for (std::size_t index{};
             index < state_->description.systemCount(); ++index)
        {
            if (state_->description.systemAt(index).instanceName() ==
                system_instance)
                return state_->findEventSlot(index, event_name);
        }
        return {};
    }

    ScriptDispatchResult ScriptBindingSession::dispatchHook(
        ScriptHookSlot hook,
        const lux_script_call_frame& frame
    ) noexcept
    {
        return dispatchHook(hook, ecs::NullEntity, frame);
    }

    ScriptDispatchResult ScriptBindingSession::dispatchHook(
        ScriptHookSlot hook,
        ecs::Entity target,
        const lux_script_call_frame& source_frame
    ) noexcept
    {
        ScriptDispatchResult total;
        if (!state_ || state_->shut_down || !hook ||
            hook.value >= state_->hooks.size())
        {
            total.status = -1;
            total.failures = 1U;
            return total;
        }
        ++state_->instrumentation.frame_builds;
        auto frame = source_frame;
        const auto& metadata = state_->hooks[hook.value];
        const std::vector<State::Handler>* handlers{};
        if (target == ecs::NullEntity)
        {
            handlers = std::addressof(metadata.global_handlers);
        }
        else
        {
            ++state_->instrumentation.entities_examined;
            auto* sidecar = state_->sidecarFor(target);
            if (!sidecar)
                return total;
            handlers = std::addressof(sidecar->hook_handlers[hook.value]);
        }
        for (const auto handler : *handlers)
        {
            const auto result = state_->invoke(
                handler,
                frame,
                metadata.cardinality == ESystemHookCardinality::SINGLE
            );
            total.calls += result.calls;
            total.failures += result.failures;
            if (result.status != 0)
                total.status = result.status;
            if (metadata.cardinality == ESystemHookCardinality::SINGLE &&
                result.calls != 0U)
                break;
        }
        return total;
    }

    ScriptDispatchResult ScriptBindingSession::dispatchEvent(
        ScriptEventSlot event,
        ecs::Entity target,
        const lux_script_call_frame& live_frame
    ) noexcept
    {
        ScriptDispatchResult total;
        if (!state_ || state_->shut_down || !event ||
            event.value >= state_->events.size())
        {
            total.status = -1;
            total.failures = 1U;
            return total;
        }
        const auto& metadata = state_->events[event.value];
        if ((target == ecs::NullEntity) !=
            (metadata.target == ESystemEventTarget::GLOBAL))
        {
            total.status = -1;
            total.failures = 1U;
            return total;
        }
        ++state_->instrumentation.frame_builds;
        auto frame = live_frame;
        const std::vector<State::Handler>* handlers{};
        if (target == ecs::NullEntity)
        {
            handlers = std::addressof(metadata.global_handlers);
        }
        else
        {
            ++state_->instrumentation.entities_examined;
            auto* sidecar = state_->sidecarFor(target);
            if (!sidecar)
                return total;
            handlers = std::addressof(sidecar->event_handlers[event.value]);
        }
        for (const auto handler : *handlers)
        {
            const auto result = state_->invoke(handler, frame, false);
            total.calls += result.calls;
            total.failures += result.failures;
            if (result.status != 0)
                total.status = result.status;
        }
        return total;
    }

    void ScriptBindingSession::clearFailures() noexcept
    {
        if (state_)
            state_->failures.clear();
    }

    std::span<const ScriptBindingFailure> ScriptBindingSession::failures() const
        noexcept
    {
        return state_
            ? std::span<const ScriptBindingFailure>{state_->failures}
            : std::span<const ScriptBindingFailure>{};
    }

    std::size_t ScriptBindingSession::instanceCount() const noexcept
    {
        return state_ ? state_->mounts.size() : 0U;
    }

    std::size_t ScriptBindingSession::preparedMethodCount() const noexcept
    {
        return state_ ? state_->preparedMethodCount() : 0U;
    }

    const ScriptBindingInstrumentation&
    ScriptBindingSession::instrumentation() const noexcept
    {
        static const ScriptBindingInstrumentation empty;
        return state_ ? state_->instrumentation : empty;
    }
}
