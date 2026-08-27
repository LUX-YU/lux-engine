#include <lux/engine/simulation/ScriptBindingSession.hpp>
#include <lux/engine/simulation/ScriptBindingCompatibility.hpp>

#include <entt/entity/entity.hpp>
#include <entt/entity/sparse_set.hpp>
#include <entt/signal/sigh.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace lux::simulation
{
    [[noreturn]] void scriptBindingContractFailure() noexcept
    {
        std::abort();
    }

    namespace
    {
        [[nodiscard]] EScriptBindingError mapBackendResult(EScriptBackendResult result) noexcept
        {
            switch (result)
            {
            case EScriptBackendResult::CAPACITY_EXCEEDED:
                return EScriptBindingError::CAPACITY_EXCEEDED;
            case EScriptBackendResult::ALLOCATION_FAILURE:
                return EScriptBindingError::ALLOCATION_FAILURE;
            case EScriptBackendResult::EXECUTABLE_CONTRACT_MISMATCH:
                return EScriptBindingError::EXECUTABLE_CONTRACT_MISMATCH;
            case EScriptBackendResult::HOST_COMPONENT_CONTRACT_MISMATCH:
                return EScriptBindingError::HOST_COMPONENT_CONTRACT_MISMATCH;
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

    const void* ScriptInstanceHostContext::read(std::uint64_t component_type) const noexcept
    {
        return attached() && api_->read ? api_->read(api_->context, self_, component_type) : nullptr;
    }

    bool ScriptInstanceHostContext::patch(std::uint64_t component_type, const void* value) const noexcept
    {
        return attached() && api_->patch && api_->patch(api_->context, self_, component_type, value);
    }

    bool ScriptInstanceHostContext::command(
        EScriptHostCommand command_value,
        std::uint64_t component_type,
        const void* value
    ) const noexcept
    {
        return attached() && api_->command && api_->command(api_->context, command_value, self_, component_type, value);
    }

    bool ScriptInstanceHostContext::componentContract(std::uint64_t component_type, ScriptHostComponentContract& result)
        const noexcept
    {
        return api_ && api_->component_contract && api_->component_contract(api_->context, component_type, result);
    }

    void ScriptInstanceHostContext::configure(const ScriptHostApi& api) noexcept
    {
        api_ = std::addressof(api);
    }

    void ScriptInstanceHostContext::attach(ecs::Entity entity) noexcept
    {
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
            void (*release_lease)(void*) noexcept {};
            std::size_t backend{};
            ScriptBackendInstance instance;
            std::vector<std::unique_ptr<PreparedMethod>> methods;
            std::optional<ScriptMountDescription> pending_authored;
            std::size_t pending_mount_order{};
            EMountState state{EMountState::CONSTRUCTING};
            EBehaviorStopReason stop_reason{EBehaviorStopReason::MOUNT_REMOVED};
            bool construct_entered{};
            bool construct_completed{};
            bool start_entered{};
            bool start_completed{};
            bool published{};
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
        };

        struct Event final
        {
            std::size_t system{};
            std::size_t member{};
            ScriptHookSlot dispatch_hook;
            ESystemEventTarget target{ESystemEventTarget::GLOBAL};
        };

        struct TargetRange final
        {
            std::uint32_t slot{};
            std::uint32_t begin{};
            std::uint32_t count{};
        };

        struct HandlerRange final
        {
            std::uint32_t begin{};
            std::uint32_t count{};
        };

        struct EntitySidecar final
        {
            ecs::Entity owner{ecs::NullEntity};
            std::uint32_t event_range_begin{};
            std::uint32_t event_range_count{};
        };

        struct DispatchIndex final
        {
            std::vector<HandlerRange> hook_ranges;
            std::vector<Handler> hook_handlers;
            std::vector<std::vector<Handler>> global_event_handlers;
            entt::basic_sparse_set<ecs::Entity> entities;
            std::vector<EntitySidecar> sidecars;
            std::vector<TargetRange> event_ranges;
            std::vector<Handler> event_handlers;
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
            : description(std::move(source)), registry(std::addressof(source_registry)), capacities(source_capacities),
              resolver(source_resolver), host_api(source_host_api),
              constructed(source_registry.on_construct<ScriptComponent>().connect<&State::onConstruct>(*this)),
              updated(source_registry.on_update<ScriptComponent>().connect<&State::onUpdate>(*this)),
              destroyed(source_registry.on_destroy<ScriptComponent>().connect<&State::onDestroy>(*this))
        {
            backends.assign(source_backends.begin(), source_backends.end());
            mounts.reserve(capacities.mount_instances);
            dirty_current.reserve(capacities.dirty_entities);
            dirty_next.reserve(capacities.dirty_entities);
            failures.reserve(capacities.failures);
            dispatch.entities.reserve(capacities.scripted_entities);
            dispatch.sidecars.reserve(capacities.scripted_entities);
            dispatch.hook_ranges.reserve(capacities.dispatch_target_ranges);
            dispatch.event_ranges.reserve(capacities.dispatch_target_ranges);
            dispatch.hook_handlers.reserve(capacities.dispatch_handlers);
            dispatch.event_handlers.reserve(capacities.dispatch_handlers);
            makeSlots();
        }

        ~State()
        {
            if (!shut_down)
                (void)shutdown();
        }

        void makeSlots()
        {
            for (std::size_t system_index{}; system_index < description.systemCount(); ++system_index)
            {
                const auto system = description.systemAt(system_index);
                for (std::size_t hook_index{}; hook_index < system.hookPointCount(); ++hook_index)
                {
                    hooks.push_back(Hook{system_index, hook_index, system.hookPointAt(hook_index).cardinality()});
                }
            }
            for (std::size_t system_index{}; system_index < description.systemCount(); ++system_index)
            {
                const auto system = description.systemAt(system_index);
                for (std::size_t event_index{}; event_index < system.eventCount(); ++event_index)
                {
                    const auto event = system.eventAt(event_index);
                    events.push_back(Event{
                        system_index,
                        event_index,
                        findHookSlot(system_index, event.dispatchHook().name()),
                        event.target()}
                    );
                }
            }
        }

        [[nodiscard]] ScriptHookSlot findHookSlot(std::size_t system_index, std::string_view name) const noexcept
        {
            for (std::size_t index{}; index < hooks.size(); ++index)
            {
                const auto& hook = hooks[index];
                if (hook.system == system_index &&
                    description.systemAt(system_index).hookPointAt(hook.member).name() == name)
                {
                    return ScriptHookSlot{static_cast<std::uint32_t>(index)};
                }
            }
            return {};
        }

        [[nodiscard]] ScriptEventSlot findEventSlot(std::size_t system_index, std::string_view name) const noexcept
        {
            for (std::size_t index{}; index < events.size(); ++index)
            {
                const auto& event = events[index];
                if (event.system == system_index &&
                    description.systemAt(system_index).eventAt(event.member).name() == name)
                {
                    return ScriptEventSlot{static_cast<std::uint32_t>(index)};
                }
            }
            return {};
        }

        [[nodiscard]] const ScriptBackendDescriptor*
        backendFor(lux::rdesc::Script::Kind kind, std::size_t& index) const noexcept
        {
            for (index = 0U; index < backends.size(); ++index)
            {
                if (backends[index].kind == kind)
                    return std::addressof(backends[index]);
            }
            return nullptr;
        }

        [[nodiscard]] const lux::rdesc::ScriptFunction*
        findFunction(const lux::asset::ScriptAssetContent& asset, lux::script::ScriptSymbolId symbol) const noexcept
        {
            const auto found = std::find_if(
                asset.description.exports.begin(),
                asset.description.exports.end(),
                [symbol](const auto& function) noexcept { return function.symbol_id == symbol; }
            );
            return found == asset.description.exports.end() ? nullptr : std::addressof(*found);
        }

        [[nodiscard]] lux::cxx::expected<SimulationSystemView, EScriptBindingError>
        resolveSystem(const SystemTypeId& type, std::string_view instance) noexcept
        {
            ++instrumentation.target_resolutions;
            if (!instance.empty())
            {
                const auto system = description.findSystem(instance);
                if (!system)
                    return lux::cxx::unexpected(EScriptBindingError::TARGET_SYSTEM_NOT_FOUND);
                if (system.type() != type)
                    return lux::cxx::unexpected(EScriptBindingError::TARGET_TYPE_MISMATCH);
                return system;
            }
            SimulationSystemView resolved;
            for (std::size_t index{}; index < description.systemCount(); ++index)
            {
                const auto candidate = description.systemAt(index);
                if (candidate.type() != type)
                    continue;
                if (resolved)
                    return lux::cxx::unexpected(EScriptBindingError::TARGET_SYSTEM_AMBIGUOUS);
                resolved = candidate;
            }
            if (!resolved)
                return lux::cxx::unexpected(EScriptBindingError::TARGET_SYSTEM_NOT_FOUND);
            return resolved;
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptBindingError> validateBinding(
            bool entity_scope,
            const lux::rdesc::ScriptFunction& function,
            const ScriptBindingDescription& binding
        ) noexcept
        {
            const auto compatibility = evaluateScriptBindingCompatibility(
                description,
                entity_scope ? lux::rdesc::EScriptModel::ENTITY_BEHAVIOR : lux::rdesc::EScriptModel::GLOBAL_MODULE,
                function,
                binding.target
            );
            switch (compatibility)
            {
            case EScriptBindingCompatibility::COMPATIBLE:
                return {};
            case EScriptBindingCompatibility::TARGET_NOT_FOUND:
                return lux::cxx::unexpected(EScriptBindingError::MEMBER_NOT_FOUND);
            case EScriptBindingCompatibility::TARGET_AMBIGUOUS:
                return lux::cxx::unexpected(EScriptBindingError::TARGET_SYSTEM_AMBIGUOUS);
            case EScriptBindingCompatibility::TARGET_TYPE_MISMATCH:
                return lux::cxx::unexpected(EScriptBindingError::TARGET_TYPE_MISMATCH);
            case EScriptBindingCompatibility::SCOPE_MISMATCH:
                return lux::cxx::unexpected(EScriptBindingError::SCOPE_MISMATCH);
            case EScriptBindingCompatibility::CARDINALITY_MISMATCH:
                return lux::cxx::unexpected(EScriptBindingError::CARDINALITY_MISMATCH);
            case EScriptBindingCompatibility::INVALID_FUNCTION:
            case EScriptBindingCompatibility::SIGNATURE_MISMATCH:
                return lux::cxx::unexpected(EScriptBindingError::SIGNATURE_MISMATCH);
            }
            return lux::cxx::unexpected(EScriptBindingError::SIGNATURE_MISMATCH);
        }

        [[nodiscard]] PreparedMethod* findMethod(MountRuntime& mount, lux::script::ScriptSymbolId symbol) noexcept
        {
            const auto found =
                std::find_if(mount.methods.begin(), mount.methods.end(), [symbol](const auto& method) noexcept {
                    return method && method->symbol == symbol;
                }
                );
            return found == mount.methods.end() ? nullptr : found->get();
        }

        [[nodiscard]] const ScriptMountDescription& effectiveAuthored(const MountRuntime& mount) const noexcept
        {
            return mount.pending_authored ? *mount.pending_authored : mount.authored;
        }

        [[nodiscard]] std::size_t effectiveMountOrder(const MountRuntime& mount) const noexcept
        {
            return mount.pending_authored ? mount.pending_mount_order : mount.mount_order;
        }

        void recordFailure(
            MountRuntime& mount,
            lux::script::ScriptSymbolId symbol,
            std::int32_t status,
            bool retire
        ) noexcept
        {
            if (retire)
            {
                mount.state = EMountState::RETIRING;
                mount.stop_reason = EBehaviorStopReason::MOUNT_REMOVED;
            }
            if (failures.size() < capacities.failures)
            {
                failures.push_back(ScriptBindingFailure{
                    EScriptBindingError::INVOCATION_FAILURE,
                    mount.authored.id,
                    symbol,
                    mount.self,
                    status}
                );
            }
        }

        [[nodiscard]] ScriptDispatchResult invoke(Handler handler, lux_script_call_frame& frame, bool single) noexcept
        {
            ScriptDispatchResult result;
            const bool is_missing_mount = handler.mount == nullptr;
            const bool is_missing_method = handler.method == nullptr;
            const bool is_inactive_mount = !is_missing_mount && handler.mount->state != EMountState::ACTIVE;
            const bool is_missing_call = !is_missing_method && !handler.method->call;
            const bool is_invalid_handler = is_missing_mount || is_missing_method || is_inactive_mount ||
                is_missing_call;
            if (is_invalid_handler)
            {
                return result;
            }
            frame.world_context = std::addressof(handler.mount->host);
            frame.user_context = handler.method->call.context;
            const auto status = handler.method->call.invoke(std::addressof(frame));
            result.calls = 1U;
            result.status = status;
            if (status != 0)
            {
                result.failures = 1U;
                recordFailure(*handler.mount, handler.method->symbol, status, true);
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
            bool success = true;
            for (const auto& binding : mount.authored.bindings)
            {
                const auto* lifecycle = std::get_if<BehaviorLifecycleBindingTarget>(std::addressof(binding.target));
                if (!lifecycle || lifecycle->point != point)
                    continue;
                auto* method = findMethod(mount, binding.function);
                if (!method || !method->call)
                    continue;
                if (point == EBehaviorLifecyclePoint::CONSTRUCT)
                    mount.construct_entered = true;
                else if (point == EBehaviorLifecyclePoint::START)
                    mount.start_entered = true;
                EBehaviorStopReason reason_value = reason;
                lux_script_value_slot reason_slot{
                    LUX_SCRIPT_VK_UINT32,
                    {},
                    sizeof(reason_value),
                    lux::script::scriptSemanticTypeId(BehaviorStopReasonCanonicalName),
                    std::addressof(reason_value)};
                lux_script_call_frame frame{
                    point == EBehaviorLifecyclePoint::STOP ? std::addressof(reason_slot) : nullptr,
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
                    recordFailure(mount, method->symbol, status, false);
                    success = false;
                    if (point != EBehaviorLifecyclePoint::STOP)
                        return false;
                }
            }
            if (point == EBehaviorLifecyclePoint::CONSTRUCT && success)
                mount.construct_completed = true;
            else if (point == EBehaviorLifecyclePoint::START && success)
                mount.start_completed = true;
            return success;
        }

        void releaseMount(MountRuntime& mount, bool invoke_stop) noexcept
        {
            const bool has_entered_lifecycle =
                mount.published || mount.construct_entered || mount.start_entered;
            const bool should_invoke_stop = invoke_stop && has_entered_lifecycle && !mount.stop_called;
            if (should_invoke_stop)
            {
                mount.stop_called = true;
                (void)invokeLifecycle(mount, EBehaviorLifecyclePoint::STOP, mount.stop_reason);
            }
            if (mount.backend < backends.size())
            {
                const auto& backend = backends[mount.backend];
                for (auto& method : mount.methods)
                {
                    if (method && backend.releaseMethod && method->call)
                    {
                        backend.releaseMethod(backend.context, mount.instance, method->call);
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

        [[nodiscard]] lux::cxx::expected<std::unique_ptr<MountRuntime>, EScriptBindingError>
        createMount(const ScriptMountDescription& authored, ecs::Entity self, std::size_t mount_order) noexcept
        {
            if (mounts.size() >= capacities.mount_instances)
                return lux::cxx::unexpected(EScriptBindingError::CAPACITY_EXCEEDED);
            ResolvedScriptAsset resolved;
            ++instrumentation.asset_resolutions;
            if (!resolver.resolve || !resolver.resolve(resolver.context, authored.script, resolved) || !resolved.asset)
            {
                return lux::cxx::unexpected(EScriptBindingError::ASSET_NOT_FOUND);
            }
            const auto release_resolved = [&]() noexcept {
                if (resolved.release)
                    resolved.release(resolved.lease);
            };
            if (!lux::rdesc::validScriptDescription(resolved.asset->description))
            {
                release_resolved();
                return lux::cxx::unexpected(EScriptBindingError::INVALID_ASSET);
            }
            const bool entity_scope = self != ecs::NullEntity;
            if (entity_scope != (resolved.asset->description.model == lux::rdesc::EScriptModel::ENTITY_BEHAVIOR))
            {
                release_resolved();
                return lux::cxx::unexpected(EScriptBindingError::SCOPE_MISMATCH);
            }
            for (const auto& binding : authored.bindings)
            {
                const auto* function = findFunction(*resolved.asset, binding.function);
                if (!function)
                {
                    release_resolved();
                    return lux::cxx::unexpected(EScriptBindingError::SYMBOL_NOT_FOUND);
                }
                const auto valid = validateBinding(entity_scope, *function, binding);
                if (!valid)
                {
                    release_resolved();
                    return lux::cxx::unexpected(valid.error());
                }
            }

            std::size_t requested_methods{};
            for (std::size_t index{}; index < authored.bindings.size(); ++index)
            {
                bool first = true;
                for (std::size_t previous{}; previous < index; ++previous)
                {
                    if (authored.bindings[previous].function == authored.bindings[index].function)
                    {
                        first = false;
                        break;
                    }
                }
                if (first)
                    ++requested_methods;
            }
            const auto prepared = preparedMethodCount();
            if (requested_methods > capacities.prepared_methods - std::min(prepared, capacities.prepared_methods))
            {
                release_resolved();
                return lux::cxx::unexpected(EScriptBindingError::CAPACITY_EXCEEDED);
            }

            std::size_t backend_index{};
            const auto* backend = backendFor(resolved.asset->description.kind(), backend_index);
            if (!backend)
            {
                release_resolved();
                return lux::cxx::unexpected(EScriptBindingError::BACKEND_NOT_AVAILABLE);
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
                runtime->methods.reserve(requested_methods);
                runtime->host.configure(host_api);
                ScriptInstanceCreateContext create_context{
                    authored.script,
                    authored.id,
                    self,
                    std::addressof(runtime->host)};
                ++instrumentation.instance_creates;
                const auto create_result =
                    backend->createInstance(backend->context, create_context, *resolved.asset, runtime->instance);
                if (create_result != EScriptBackendResult::SUCCESS)
                {
                    releaseMount(*runtime, false);
                    return lux::cxx::unexpected(mapBackendResult(create_result));
                }
                runtime->host.attach(self);
                for (const auto& binding : authored.bindings)
                {
                    if (findMethod(*runtime, binding.function))
                        continue;
                    const auto* function = findFunction(*resolved.asset, binding.function);
                    lux::script::BoundScriptCall call;
                    ++instrumentation.method_prepares;
                    const auto prepare_result =
                        backend->prepareMethod(backend->context, runtime->instance, *function, call);
                    if (prepare_result != EScriptBackendResult::SUCCESS || !call)
                    {
                        releaseMount(*runtime, false);
                        return lux::cxx::unexpected(mapBackendResult(prepare_result));
                    }
                    runtime->methods.push_back(
                        std::make_unique<PreparedMethod>(PreparedMethod{binding.function, call})
                    );
                }
                return runtime;
            }
            catch (const std::bad_alloc&)
            {
                if (runtime)
                    releaseMount(*runtime, false);
                release_resolved();
                return lux::cxx::unexpected(EScriptBindingError::ALLOCATION_FAILURE);
            }
        }

        [[nodiscard]] std::size_t preparedMethodCount() const noexcept
        {
            std::size_t count{};
            for (const auto& mount : mounts)
                count += mount->methods.size();
            return count;
        }

        [[nodiscard]] std::size_t missingPreparedMethodCount(
            ecs::Entity self,
            const std::vector<ScriptMountDescription>& authored_mounts
        ) noexcept
        {
            std::size_t missing{};
            for (const auto& authored : authored_mounts)
            {
                auto* runtime = findMount(self, authored.id);
                const bool reusable = runtime && runtime->authored.script == authored.script;
                for (std::size_t index{}; index < authored.bindings.size(); ++index)
                {
                    bool first = true;
                    for (std::size_t previous{}; previous < index; ++previous)
                    {
                        if (authored.bindings[previous].function == authored.bindings[index].function)
                        {
                            first = false;
                            break;
                        }
                    }
                    if (!first || (reusable && findMethod(*runtime, authored.bindings[index].function)))
                    {
                        continue;
                    }
                    ++missing;
                }
            }
            return missing;
        }

        [[nodiscard]] MountRuntime* findMount(ecs::Entity self, ScriptMountId id) const noexcept
        {
            for (const auto& mount : mounts)
            {
                const bool is_live = mount->state == EMountState::CONSTRUCTING || mount->state == EMountState::ACTIVE;
                const bool is_matching_self = mount->self == self;
                const bool is_matching_id = mount->authored.id == id;
                const bool is_match = is_live && is_matching_self && is_matching_id;
                if (is_match)
                {
                    return mount.get();
                }
            }
            return nullptr;
        }

        void markRemoved(ecs::Entity self, const ScriptComponent* desired, EBehaviorStopReason reason) noexcept
        {
            for (auto& runtime : mounts)
            {
                if (runtime->self != self || runtime->state == EMountState::DEAD)
                    continue;
                const auto found = desired ? std::find_if(
                                                 desired->mounts.begin(),
                                                 desired->mounts.end(),
                                                 [&](const auto& mount) noexcept {
                                                     return mount.id == runtime->authored.id &&
                                                            mount.script == runtime->authored.script;
                                                 }
                )
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
                    [](const auto& mount) noexcept { return mount->state == EMountState::DEAD; }
                ),
                mounts.end()
            );
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptBindingError>
        reconcileMount(MountRuntime& runtime, const ScriptMountDescription& authored, std::size_t mount_order) noexcept
        {
            const bool entity_scope = runtime.self != ecs::NullEntity;
            for (const auto& binding : authored.bindings)
            {
                const auto* function = findFunction(*runtime.asset, binding.function);
                if (!function)
                    return lux::cxx::unexpected(EScriptBindingError::SYMBOL_NOT_FOUND);
                const auto valid = validateBinding(entity_scope, *function, binding);
                if (!valid)
                    return lux::cxx::unexpected(valid.error());
            }
            const auto& backend = backends[runtime.backend];
            std::size_t missing{};
            for (std::size_t index{}; index < authored.bindings.size(); ++index)
            {
                if (findMethod(runtime, authored.bindings[index].function))
                    continue;
                bool first = true;
                for (std::size_t previous{}; previous < index; ++previous)
                {
                    if (authored.bindings[previous].function == authored.bindings[index].function)
                    {
                        first = false;
                        break;
                    }
                }
                if (first)
                    ++missing;
            }
            const auto prepared = preparedMethodCount();
            if (missing > capacities.prepared_methods - std::min(prepared, capacities.prepared_methods))
            {
                return lux::cxx::unexpected(EScriptBindingError::CAPACITY_EXCEEDED);
            }
            const auto original_size = runtime.methods.size();
            const auto rollback = [&]() noexcept {
                while (runtime.methods.size() > original_size)
                {
                    auto& method = runtime.methods.back();
                    if (method && backend.releaseMethod && method->call)
                    {
                        backend.releaseMethod(backend.context, runtime.instance, method->call);
                    }
                    runtime.methods.pop_back();
                }
            };
            try
            {
                runtime.methods.reserve(original_size + missing);
                for (const auto& binding : authored.bindings)
                {
                    if (findMethod(runtime, binding.function))
                        continue;
                    const auto* function = findFunction(*runtime.asset, binding.function);
                    lux::script::BoundScriptCall call;
                    ++instrumentation.method_prepares;
                    const auto result = backend.prepareMethod(backend.context, runtime.instance, *function, call);
                    if (result != EScriptBackendResult::SUCCESS || !call)
                    {
                        rollback();
                        return lux::cxx::unexpected(mapBackendResult(result));
                    }
                    try
                    {
                        runtime.methods.push_back(
                            std::make_unique<PreparedMethod>(PreparedMethod{binding.function, call})
                        );
                    }
                    catch (const std::bad_alloc&)
                    {
                        if (backend.releaseMethod)
                        {
                            backend.releaseMethod(backend.context, runtime.instance, call);
                        }
                        rollback();
                        return lux::cxx::unexpected(EScriptBindingError::ALLOCATION_FAILURE);
                    }
                }
                runtime.pending_authored = authored;
                runtime.pending_mount_order = mount_order;
                return {};
            }
            catch (const std::bad_alloc&)
            {
                rollback();
                return lux::cxx::unexpected(EScriptBindingError::ALLOCATION_FAILURE);
            }
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptBindingError>
        upsertOwner(ecs::Entity self, const std::vector<ScriptMountDescription>& authored_mounts) noexcept
        {
            if (!validScriptMountList(authored_mounts))
                return lux::cxx::unexpected(EScriptBindingError::INVALID_ASSET);
            for (std::size_t index{}; index < authored_mounts.size(); ++index)
            {
                const auto& authored = authored_mounts[index];
                if (auto* runtime = findMount(self, authored.id);
                    runtime && runtime->authored.script == authored.script)
                {
                    const auto reconciled = reconcileMount(*runtime, authored, index);
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

        [[nodiscard]] lux::cxx::expected<DispatchIndex, EScriptBindingError>
        makeDispatch(bool include_constructing = true, bool use_pending_authored = true) noexcept
        {
            try
            {
                struct PendingHook final
                {
                    std::uint32_t slot{};
                    Handler handler;
                    std::size_t sequence{};
                };

                struct PendingEvent final
                {
                    ecs::Entity owner{ecs::NullEntity};
                    std::uint32_t slot{};
                    Handler handler;
                    std::size_t sequence{};
                };

                DispatchIndex shadow;
                shadow.hook_ranges.resize(hooks.size());
                shadow.global_event_handlers.resize(events.size());
                shadow.entities.reserve(capacities.scripted_entities);
                shadow.sidecars.reserve(capacities.scripted_entities);
                shadow.event_ranges.reserve(capacities.dispatch_target_ranges);
                shadow.hook_handlers.reserve(capacities.dispatch_handlers);
                shadow.event_handlers.reserve(capacities.dispatch_handlers);

                std::vector<PendingHook> pending_hooks;
                std::vector<PendingEvent> pending_events;
                pending_hooks.reserve(capacities.dispatch_handlers);
                pending_events.reserve(capacities.dispatch_handlers);

                std::vector<MountRuntime*> ordered;
                ordered.reserve(mounts.size());
                for (auto& mount : mounts)
                {
                    if (mount->state == EMountState::ACTIVE ||
                        (include_constructing && mount->state == EMountState::CONSTRUCTING))
                        ordered.push_back(mount.get());
                }
                std::sort(ordered.begin(), ordered.end(), [&](const auto* left, const auto* right) noexcept {
                    if ((left->self == ecs::NullEntity) != (right->self == ecs::NullEntity))
                    {
                        return left->self == ecs::NullEntity;
                    }
                    if (left->self != right->self)
                        return ecs::entityBits(left->self) < ecs::entityBits(right->self);
                    const auto left_order = use_pending_authored ? effectiveMountOrder(*left) : left->mount_order;
                    const auto right_order = use_pending_authored ? effectiveMountOrder(*right) : right->mount_order;
                    return left_order < right_order;
                }
                );

                std::size_t sequence{};
                std::size_t handler_count{};
                for (auto* mount : ordered)
                {
                    const auto& authored = use_pending_authored ? effectiveAuthored(*mount) : mount->authored;
                    for (const auto& binding : authored.bindings)
                    {
                        auto* method = findMethod(*mount, binding.function);
                        if (!method)
                            return lux::cxx::unexpected(EScriptBindingError::SYMBOL_NOT_FOUND);
                        const Handler handler{mount, method};
                        const auto indexed = std::visit(
                            [&](const auto& target) noexcept -> bool {
                                using Target = std::remove_cvref_t<decltype(target)>;
                                if constexpr (std::is_same_v<Target, SystemHookBindingTarget>)
                                {
                                    auto system = resolveSystem(target.system_type, target.system_instance);
                                    if (!system)
                                        return false;
                                    ScriptHookSlot resolved_slot;
                                    for (std::size_t index{}; index < description.systemCount(); ++index)
                                    {
                                        if (description.systemAt(index).instanceName() == system->instanceName())
                                        {
                                            resolved_slot = findHookSlot(index, target.hook);
                                            break;
                                        }
                                    }
                                    if (!resolved_slot)
                                        return false;
                                    pending_hooks.push_back(PendingHook{resolved_slot.value, handler, sequence++});
                                }
                                else if constexpr (std::is_same_v<Target, SystemEventBindingTarget>)
                                {
                                    auto system = resolveSystem(target.system_type, target.system_instance);
                                    if (!system)
                                        return false;
                                    ScriptEventSlot resolved_slot;
                                    for (std::size_t index{}; index < description.systemCount(); ++index)
                                    {
                                        if (description.systemAt(index).instanceName() == system->instanceName())
                                        {
                                            resolved_slot = findEventSlot(index, target.event);
                                            break;
                                        }
                                    }
                                    if (!resolved_slot)
                                        return false;
                                    if (mount->self != ecs::NullEntity)
                                    {
                                        pending_events.push_back(
                                            PendingEvent{mount->self, resolved_slot.value, handler, sequence++}
                                        );
                                    }
                                    else
                                    {
                                        shadow.global_event_handlers[resolved_slot.value].push_back(handler);
                                    }
                                }
                                return true;
                            },
                            binding.target
                        );
                        if (!indexed)
                        {
                            const bool duplicate_single = std::visit(
                                [&](const auto& target) noexcept {
                                    using Target = std::remove_cvref_t<decltype(target)>;
                                    if constexpr (!std::is_same_v<Target, SystemHookBindingTarget>)
                                    {
                                        return false;
                                    }
                                    else
                                    {
                                        const auto system = resolveSystem(target.system_type, target.system_instance);
                                        if (!system)
                                            return false;
                                        for (std::size_t index{}; index < description.systemCount(); ++index)
                                        {
                                            if (description.systemAt(index).instanceName() != system->instanceName())
                                            {
                                                continue;
                                            }
                                            const auto slot = findHookSlot(index, target.hook);
                                            return slot &&
                                                   hooks[slot.value].cardinality == ESystemHookCardinality::SINGLE &&
                                                   mount->self == ecs::NullEntity;
                                        }
                                        return false;
                                    }
                                },
                                binding.target
                            );
                            return lux::cxx::unexpected(
                                duplicate_single ? EScriptBindingError::SINGLE_HOOK_MULTIPLE_HANDLERS
                                                 : EScriptBindingError::MEMBER_NOT_FOUND
                            );
                        }
                        ++handler_count;
                        if (handler_count > capacities.dispatch_handlers)
                        {
                            return lux::cxx::unexpected(EScriptBindingError::CAPACITY_EXCEEDED);
                        }
                    }
                }

                std::stable_sort(
                    pending_hooks.begin(),
                    pending_hooks.end(),
                    [](const PendingHook& left, const PendingHook& right) noexcept { return left.slot < right.slot; }
                );
                std::size_t hook_cursor{};
                while (hook_cursor < pending_hooks.size())
                {
                    const auto slot = pending_hooks[hook_cursor].slot;
                    if (slot >= shadow.hook_ranges.size())
                    {
                        return lux::cxx::unexpected(EScriptBindingError::MEMBER_NOT_FOUND);
                    }
                    const auto begin = shadow.hook_handlers.size();
                    while (hook_cursor < pending_hooks.size() && pending_hooks[hook_cursor].slot == slot)
                    {
                        shadow.hook_handlers.push_back(pending_hooks[hook_cursor].handler);
                        ++hook_cursor;
                    }
                    const auto count = shadow.hook_handlers.size() - begin;
                    if (hooks[slot].cardinality == ESystemHookCardinality::SINGLE && count > 1U)
                    {
                        return lux::cxx::unexpected(EScriptBindingError::SINGLE_HOOK_MULTIPLE_HANDLERS);
                    }
                    shadow.hook_ranges[slot] =
                        HandlerRange{static_cast<std::uint32_t>(begin), static_cast<std::uint32_t>(count)};
                }

                const auto pending_less = [](const PendingEvent& left, const PendingEvent& right) noexcept {
                    const auto left_bits = ecs::entityBits(left.owner);
                    const auto right_bits = ecs::entityBits(right.owner);
                    if (left_bits != right_bits)
                        return left_bits < right_bits;
                    if (left.slot != right.slot)
                        return left.slot < right.slot;
                    return left.sequence < right.sequence;
                };
                std::sort(pending_events.begin(), pending_events.end(), pending_less);

                std::vector<ecs::Entity> owners;
                owners.reserve(capacities.scripted_entities);
                for (const auto& pending : pending_events)
                {
                    if (!owners.empty() && owners.back() == pending.owner)
                    {
                        continue;
                    }
                    if (owners.size() >= capacities.scripted_entities)
                    {
                        return lux::cxx::unexpected(EScriptBindingError::CAPACITY_EXCEEDED);
                    }
                    owners.push_back(pending.owner);
                    shadow.entities.push(pending.owner);
                    shadow.sidecars.push_back(EntitySidecar{pending.owner});
                }

                const auto flatten =
                    [&](const std::vector<PendingEvent>& pending) -> lux::cxx::expected<void, EScriptBindingError> {
                    auto& ranges = shadow.event_ranges;
                    auto& handlers = shadow.event_handlers;
                    std::size_t cursor{};
                    while (cursor < pending.size())
                    {
                        const auto owner = pending[cursor].owner;
                        if (!shadow.entities.contains(owner))
                        {
                            return lux::cxx::unexpected(EScriptBindingError::INVALID_ENTITY);
                        }
                        const auto sidecar_index = shadow.entities.index(owner);
                        if (sidecar_index >= shadow.sidecars.size())
                        {
                            return lux::cxx::unexpected(EScriptBindingError::INVALID_ENTITY);
                        }
                        auto& sidecar = shadow.sidecars[sidecar_index];
                        auto& range_begin = sidecar.event_range_begin;
                        auto& range_count = sidecar.event_range_count;
                        range_begin = static_cast<std::uint32_t>(ranges.size());
                        while (cursor < pending.size() && pending[cursor].owner == owner)
                        {
                            if (shadow.event_ranges.size() >= capacities.dispatch_target_ranges)
                            {
                                return lux::cxx::unexpected(EScriptBindingError::CAPACITY_EXCEEDED);
                            }
                            const auto slot = pending[cursor].slot;
                            const auto begin = handlers.size();
                            while (cursor < pending.size() && pending[cursor].owner == owner &&
                                   pending[cursor].slot == slot)
                            {
                                handlers.push_back(pending[cursor].handler);
                                ++cursor;
                            }
                            ranges.push_back(TargetRange{
                                slot,
                                static_cast<std::uint32_t>(begin),
                                static_cast<std::uint32_t>(handlers.size() - begin)}
                            );
                            ++range_count;
                        }
                    }
                    return {};
                };
                if (auto flattened = flatten(pending_events); !flattened)
                    return lux::cxx::unexpected(flattened.error());

                return shadow;
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(EScriptBindingError::ALLOCATION_FAILURE);
            }
        }

        void enqueue(ecs::Entity entity, EBehaviorStopReason reason) noexcept
        {
            if (dirty_next.size() >= capacities.dirty_entities)
            {
                full_resync_next = true;
                return;
            }
            dirty_next.push_back(DirtyEntity{entity, reason});
        }

        void onConstruct(ecs::Registry&, ecs::Entity entity) noexcept
        {
            enqueue(entity, EBehaviorStopReason::MOUNT_REMOVED);
        }

        void onUpdate(ecs::Registry& source, ecs::Entity entity) noexcept
        {
            const auto* component = source.try_get<ScriptComponent>(entity);
            markRemoved(entity, component, EBehaviorStopReason::MOUNT_REMOVED);
            enqueue(entity, EBehaviorStopReason::MOUNT_REMOVED);
        }

        void onDestroy(ecs::Registry&, ecs::Entity entity) noexcept
        {
            markRemoved(entity, nullptr, EBehaviorStopReason::ENTITY_DESTROYED);
            enqueue(entity, EBehaviorStopReason::ENTITY_DESTROYED);
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptBindingError> prepareInitial() noexcept
        {
            if (shut_down)
                return lux::cxx::unexpected(EScriptBindingError::SESSION_SHUT_DOWN);
            if (prepared_once)
                return {};
            try
            {
                std::vector<ScriptMountDescription> global_mounts;
                global_mounts.reserve(description.globalScriptMountCount());
                for (std::size_t index{}; index < description.globalScriptMountCount(); ++index)
                {
                    const auto view = description.globalScriptMountAt(index);
                    ScriptMountDescription mount{view.id(), view.script(), {}};
                    mount.bindings.reserve(view.bindingCount());
                    for (std::size_t binding{}; binding < view.bindingCount(); ++binding)
                    {
                        mount.bindings.push_back(*view.bindingAt(binding));
                    }
                    global_mounts.push_back(std::move(mount));
                }
                std::vector<ecs::Entity> entities;
                const auto view = registry->view<ScriptComponent>();
                for (const auto entity : view)
                    entities.push_back(entity);
                std::sort(entities.begin(), entities.end(), [](auto left, auto right) noexcept {
                    return ecs::entityBits(left) < ecs::entityBits(right);
                }
                );
                std::size_t required_methods{};
                const auto account = [&](ecs::Entity owner,
                                         const std::vector<ScriptMountDescription>& owner_mounts) noexcept {
                    const auto missing = missingPreparedMethodCount(owner, owner_mounts);
                    if (missing > capacities.prepared_methods - std::min(required_methods, capacities.prepared_methods))
                    {
                        return false;
                    }
                    required_methods += missing;
                    return true;
                };
                if (!account(ecs::NullEntity, global_mounts))
                {
                    return lux::cxx::unexpected(EScriptBindingError::CAPACITY_EXCEEDED);
                }
                for (const auto entity : entities)
                {
                    if (!account(entity, registry->get<ScriptComponent>(entity).mounts))
                    {
                        return lux::cxx::unexpected(EScriptBindingError::CAPACITY_EXCEEDED);
                    }
                }
                auto result = upsertOwner(ecs::NullEntity, global_mounts);
                if (!result)
                {
                    rollbackStaged();
                    return result;
                }
                for (const auto entity : entities)
                {
                    const auto& component = registry->get<ScriptComponent>(entity);
                    result = upsertOwner(entity, component.mounts);
                    if (!result)
                    {
                        rollbackStaged();
                        return result;
                    }
                }
                result = activateAndPublish();
                if (!result)
                    return result;
                dirty_current.clear();
                full_resync_current = false;
                prepared_once = true;
                return {};
            }
            catch (const std::bad_alloc&)
            {
                rollbackStaged();
                full_resync_next = true;
                return lux::cxx::unexpected(EScriptBindingError::ALLOCATION_FAILURE);
            }
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptBindingError> applyMutations() noexcept
        {
            if (shut_down)
                return lux::cxx::unexpected(EScriptBindingError::SESSION_SHUT_DOWN);
            if (!prepared_once)
                return prepareInitial();
            try
            {
                dirty_current.clear();
                dirty_current.swap(dirty_next);
                full_resync_current = full_resync_next;
                full_resync_next = false;
                if (full_resync_current)
                {
                    for (auto& runtime : mounts)
                    {
                        if (runtime->self == ecs::NullEntity || runtime->state == EMountState::DEAD)
                            continue;
                        const auto* component = registry->valid(runtime->self)
                                                    ? registry->try_get<ScriptComponent>(runtime->self)
                                                    : nullptr;
                        markRemoved(
                            runtime->self,
                            component,
                            registry->valid(runtime->self) ? EBehaviorStopReason::MOUNT_REMOVED
                                                           : EBehaviorStopReason::ENTITY_DESTROYED
                        );
                    }
                    std::vector<ecs::Entity> entities;
                    const auto view = registry->view<ScriptComponent>();
                    for (const auto entity : view)
                        entities.push_back(entity);
                    std::sort(entities.begin(), entities.end(), [](auto left, auto right) noexcept {
                        return ecs::entityBits(left) < ecs::entityBits(right);
                    }
                    );
                    std::size_t required_methods = preparedMethodCount();
                    for (const auto entity : entities)
                    {
                        const auto missing =
                            missingPreparedMethodCount(entity, registry->get<ScriptComponent>(entity).mounts);
                        if (missing >
                            capacities.prepared_methods - std::min(required_methods, capacities.prepared_methods))
                        {
                            rollbackStaged();
                            full_resync_next = true;
                            return lux::cxx::unexpected(EScriptBindingError::CAPACITY_EXCEEDED);
                        }
                        required_methods += missing;
                    }
                    for (const auto entity : entities)
                    {
                        const auto result = upsertOwner(entity, registry->get<ScriptComponent>(entity).mounts);
                        if (!result)
                        {
                            rollbackStaged();
                            full_resync_next = true;
                            return result;
                        }
                    }
                }
                else
                {
                    std::sort(
                        dirty_current.begin(),
                        dirty_current.end(),
                        [](const auto& left, const auto& right) noexcept {
                            const auto left_bits = ecs::entityBits(left.entity);
                            const auto right_bits = ecs::entityBits(right.entity);
                            if (left_bits != right_bits)
                                return left_bits < right_bits;
                            return static_cast<std::uint32_t>(left.reason) > static_cast<std::uint32_t>(right.reason);
                        }
                    );
                    dirty_current.erase(
                        std::unique(
                            dirty_current.begin(),
                            dirty_current.end(),
                            [](const auto& left, const auto& right) noexcept { return left.entity == right.entity; }
                        ),
                        dirty_current.end()
                    );
                    for (const auto change : dirty_current)
                    {
                        const auto* component = registry->valid(change.entity)
                                                    ? registry->try_get<ScriptComponent>(change.entity)
                                                    : nullptr;
                        markRemoved(change.entity, component, change.reason);
                    }
                    std::size_t required_methods = preparedMethodCount();
                    for (const auto change : dirty_current)
                    {
                        if (!registry->valid(change.entity))
                            continue;
                        const auto* component = registry->try_get<ScriptComponent>(change.entity);
                        if (!component)
                            continue;
                        const auto missing = missingPreparedMethodCount(change.entity, component->mounts);
                        if (missing >
                            capacities.prepared_methods - std::min(required_methods, capacities.prepared_methods))
                        {
                            rollbackStaged();
                            full_resync_next = true;
                            return lux::cxx::unexpected(EScriptBindingError::CAPACITY_EXCEEDED);
                        }
                        required_methods += missing;
                    }
                    for (const auto change : dirty_current)
                    {
                        if (!registry->valid(change.entity))
                            continue;
                        const auto* component = registry->try_get<ScriptComponent>(change.entity);
                        if (!component)
                            continue;
                        const auto result = upsertOwner(change.entity, component->mounts);
                        if (!result)
                        {
                            rollbackStaged();
                            full_resync_next = true;
                            return result;
                        }
                    }
                }
                auto result = activateAndPublish();
                if (!result)
                    return result;
                dirty_current.clear();
                full_resync_current = false;
                return {};
            }
            catch (const std::bad_alloc&)
            {
                rollbackStaged();
                full_resync_next = true;
                return lux::cxx::unexpected(EScriptBindingError::ALLOCATION_FAILURE);
            }
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptBindingError> shutdown() noexcept
        {
            if (shut_down)
                return {};
            for (auto& mount : mounts)
            {
                if (mount->state != EMountState::DEAD)
                {
                    mount->state = EMountState::RETIRING;
                    mount->stop_reason = EBehaviorStopReason::SIMULATION_STOPPED;
                }
            }
            destroyRetired();
            dispatch = DispatchIndex{};
            shut_down = true;
            return {};
        }

        [[nodiscard]] EntitySidecar* sidecarFor(ecs::Entity entity) noexcept
        {
            if (!dispatch.entities.contains(entity))
                return nullptr;
            const auto sidecar_index = dispatch.entities.index(entity);
            if (sidecar_index >= dispatch.sidecars.size())
                return nullptr;
            auto& sidecar = dispatch.sidecars[sidecar_index];
            return sidecar.owner == entity ? std::addressof(sidecar) : nullptr;
        }

        [[nodiscard]] std::vector<MountRuntime*> orderedConstructing()
        {
            std::vector<MountRuntime*> ordered;
            ordered.reserve(mounts.size());
            for (auto& mount : mounts)
            {
                if (mount->state == EMountState::CONSTRUCTING)
                    ordered.push_back(mount.get());
            }
            std::sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) noexcept {
                if ((left->self == ecs::NullEntity) != (right->self == ecs::NullEntity))
                {
                    return left->self == ecs::NullEntity;
                }
                if (left->self != right->self)
                {
                    return ecs::entityBits(left->self) < ecs::entityBits(right->self);
                }
                return left->mount_order < right->mount_order;
            }
            );
            return ordered;
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptBindingError> constructPending() noexcept
        {
            try
            {
                const auto ordered = orderedConstructing();
                for (auto* mount : ordered)
                {
                    if (!invokeLifecycle(*mount, EBehaviorLifecyclePoint::CONSTRUCT))
                    {
                        return lux::cxx::unexpected(EScriptBindingError::INVOCATION_FAILURE);
                    }
                }
                return {};
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(EScriptBindingError::ALLOCATION_FAILURE);
            }
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptBindingError> startPending() noexcept
        {
            try
            {
                const auto ordered = orderedConstructing();
                for (auto* mount : ordered)
                {
                    if (!invokeLifecycle(*mount, EBehaviorLifecyclePoint::START))
                    {
                        return lux::cxx::unexpected(EScriptBindingError::INVOCATION_FAILURE);
                    }
                }
                for (auto* mount : ordered)
                    mount->state = EMountState::ACTIVE;
                return {};
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(EScriptBindingError::ALLOCATION_FAILURE);
            }
        }

        void rollbackStaged() noexcept
        {
            for (auto& mount : mounts)
            {
                if (!mount->published)
                {
                    mount->stop_reason = EBehaviorStopReason::INITIALIZATION_FAILED;
                    releaseMount(*mount, true);
                    continue;
                }
                if (!mount->pending_authored)
                    continue;
                const auto& backend = backends[mount->backend];
                std::erase_if(mount->methods, [&](const auto& method) noexcept {
                    const auto committed = std::any_of(
                        mount->authored.bindings.begin(),
                        mount->authored.bindings.end(),
                        [&](const auto& binding) noexcept { return binding.function == method->symbol; }
                    );
                    if (!committed && backend.releaseMethod && method->call)
                    {
                        backend.releaseMethod(backend.context, mount->instance, method->call);
                    }
                    return !committed;
                }
                );
                mount->pending_authored.reset();
            }
            mounts.erase(
                std::remove_if(
                    mounts.begin(),
                    mounts.end(),
                    [](const auto& mount) noexcept { return mount->state == EMountState::DEAD; }
                ),
                mounts.end()
            );
        }

        void publishDispatch(DispatchIndex&& shadow) noexcept
        {
            instrumentation.target_ranges_built =
                static_cast<std::size_t>(std::count_if(
                    shadow.hook_ranges.begin(),
                    shadow.hook_ranges.end(),
                    [](const HandlerRange& range) noexcept { return range.count != 0U; })) +
                shadow.event_ranges.size();
            instrumentation.dispatch_handlers_built = shadow.hook_handlers.size() + shadow.event_handlers.size();
            dispatch = std::move(shadow);
        }

        void commitPending() noexcept
        {
            for (auto& mount : mounts)
            {
                if (mount->pending_authored)
                {
                    mount->authored = std::move(*mount->pending_authored);
                    mount->mount_order = mount->pending_mount_order;
                    mount->pending_authored.reset();
                }
                if (mount->state != EMountState::ACTIVE)
                    continue;
                mount->published = true;
                const auto& backend = backends[mount->backend];
                std::erase_if(mount->methods, [&](const auto& method) noexcept {
                    const auto retained = std::any_of(
                        mount->authored.bindings.begin(),
                        mount->authored.bindings.end(),
                        [&](const auto& binding) noexcept { return method && binding.function == method->symbol; }
                    );
                    const bool is_releaseable_method = method != nullptr && backend.releaseMethod != nullptr &&
                        method->call;
                    const bool should_release_method = !retained && is_releaseable_method;
                    if (should_release_method)
                    {
                        backend.releaseMethod(backend.context, mount->instance, method->call);
                    }
                    return !retained;
                }
                );
            }
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptBindingError> activateAndPublish() noexcept
        {
            auto constructed_result = constructPending();
            if (!constructed_result)
            {
                rollbackStaged();
                full_resync_next = true;
                return constructed_result;
            }
            auto candidate = makeDispatch(true, true);
            if (!candidate)
            {
                rollbackStaged();
                full_resync_next = true;
                return lux::cxx::unexpected(candidate.error());
            }
            auto fallback = makeDispatch(false, false);
            if (!fallback)
            {
                rollbackStaged();
                full_resync_next = true;
                return lux::cxx::unexpected(fallback.error());
            }
            destroyRetired();
            auto started = startPending();
            if (!started)
            {
                rollbackStaged();
                publishDispatch(std::move(*fallback));
                full_resync_next = true;
                return started;
            }
            publishDispatch(std::move(*candidate));
            commitPending();
            return {};
        }

        [[nodiscard]] std::span<const Handler> eventHandlersFor(ecs::Entity entity, std::uint32_t slot) noexcept
        {
            const auto* sidecar = sidecarFor(entity);
            if (!sidecar)
                return {};
            const auto& ranges = dispatch.event_ranges;
            const auto& handlers = dispatch.event_handlers;
            const auto begin = sidecar->event_range_begin;
            const auto count = sidecar->event_range_count;
            if (begin > ranges.size() || count > ranges.size() - begin)
                return {};
            const auto first = ranges.begin() + begin;
            const auto last = first + count;
            ++instrumentation.target_range_lookups;
            const auto found =
                std::lower_bound(first, last, slot, [](const TargetRange& range, std::uint32_t value) noexcept {
                    return range.slot < value;
                }
                );
            const bool is_missing_range = found == last;
            const bool is_wrong_slot = !is_missing_range && found->slot != slot;
            const bool is_invalid_begin = !is_missing_range && found->begin > handlers.size();
            const bool is_invalid_count = !is_missing_range && !is_invalid_begin &&
                found->count > handlers.size() - found->begin;
            const bool is_invalid_range = is_missing_range || is_wrong_slot || is_invalid_begin || is_invalid_count;
            if (is_invalid_range)
            {
                return {};
            }
            return {handlers.data() + found->begin, found->count};
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
        DispatchIndex dispatch;
        std::vector<DirtyEntity> dirty_current;
        std::vector<DirtyEntity> dirty_next;
        std::vector<ScriptBindingFailure> failures;
        ScriptBindingInstrumentation instrumentation;
        entt::scoped_connection constructed;
        entt::scoped_connection updated;
        entt::scoped_connection destroyed;
        bool full_resync_current{};
        bool full_resync_next{};
        bool prepared_once{};
        bool shut_down{};
    };

    ScriptBindingSession::ScriptBindingSession(std::unique_ptr<State> state) noexcept : state_(std::move(state))
    {
    }

    lux::cxx::expected<ScriptBindingSession, EScriptBindingError> ScriptBindingSession::create(
        SimulationDescription description,
        ecs::Registry& registry,
        ScriptBindingCapacities capacities,
        ScriptAssetResolver resolver,
        std::span<const ScriptBackendDescriptor> backends,
        ScriptHostApi host_api
    ) noexcept
    {
        const bool is_missing_resolver = resolver.resolve == nullptr;
        const bool is_invalid_mount_capacity = capacities.mount_instances == 0U ||
            capacities.prepared_methods == 0U;
        const bool is_invalid_entity_capacity = capacities.scripted_entities == 0U ||
            capacities.dispatch_target_ranges == 0U || capacities.dispatch_handlers == 0U ||
            capacities.dirty_entities == 0U;
        const bool is_invalid_capacity = is_missing_resolver || is_invalid_mount_capacity ||
            is_invalid_entity_capacity;
        if (is_invalid_capacity)
        {
            return lux::cxx::unexpected(EScriptBindingError::CAPACITY_EXCEEDED);
        }
        for (std::size_t index{}; index < backends.size(); ++index)
        {
            const auto& backend = backends[index];
            const bool is_unknown_kind = backend.kind == lux::rdesc::Script::Kind::UNKNOWN;
            const bool is_missing_create = !backend.createInstance;
            const bool is_missing_prepare = !backend.prepareMethod;
            const bool is_missing_destroy = !backend.destroyInstance;
            const bool is_invalid_backend = is_unknown_kind || is_missing_create || is_missing_prepare ||
                is_missing_destroy;
            if (is_invalid_backend)
            {
                return lux::cxx::unexpected(EScriptBindingError::BACKEND_CONSTRUCTION_FAILURE);
            }
            for (std::size_t previous{}; previous < index; ++previous)
            {
                if (backends[previous].kind == backend.kind)
                {
                    return lux::cxx::unexpected(EScriptBindingError::DUPLICATE_BACKEND_KIND);
                }
            }
        }
        try
        {
            return ScriptBindingSession(
                std::make_unique<State>(std::move(description), registry, capacities, resolver, backends, host_api)
            );
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(EScriptBindingError::ALLOCATION_FAILURE);
        }
    }

    ScriptBindingSession::ScriptBindingSession(ScriptBindingSession&&) noexcept = default;
    ScriptBindingSession& ScriptBindingSession::operator=(ScriptBindingSession&&) noexcept = default;
    ScriptBindingSession::~ScriptBindingSession() = default;

    lux::cxx::expected<void, EScriptBindingError> ScriptBindingSession::prepare() noexcept
    {
        return state_ ? state_->prepareInitial()
                      : lux::cxx::expected<void, EScriptBindingError>{
                            lux::cxx::unexpected(EScriptBindingError::INVALID_SLOT)};
    }

    lux::cxx::expected<void, EScriptBindingError> ScriptBindingSession::applyQuiescentMutations() noexcept
    {
        return state_ ? state_->applyMutations()
                      : lux::cxx::expected<void, EScriptBindingError>{
                            lux::cxx::unexpected(EScriptBindingError::INVALID_SLOT)};
    }

    lux::cxx::expected<void, EScriptBindingError> ScriptBindingSession::shutdown() noexcept
    {
        return state_ ? state_->shutdown() : lux::cxx::expected<void, EScriptBindingError>{};
    }

    ScriptHookSlot
    ScriptBindingSession::hookSlot(std::string_view system_instance, std::string_view hook_name) const noexcept
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

    ScriptEventSlot
    ScriptBindingSession::eventSlot(std::string_view system_instance, std::string_view event_name) const noexcept
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

    ScriptDispatchResult
    ScriptBindingSession::dispatchHook(ScriptHookSlot hook, const lux_script_call_frame& source_frame) noexcept
    {
        ScriptDispatchResult total;
        const bool is_missing_state = state_ == nullptr;
        const bool is_shutdown = !is_missing_state && state_->shut_down;
        const bool is_missing_hook = !hook;
        const bool is_out_of_range = !is_missing_state && hook.value >= state_->hooks.size();
        const bool is_invalid_hook = is_missing_state || is_shutdown || is_missing_hook || is_out_of_range;
        if (is_invalid_hook)
        {
            total.status = -1;
            total.failures = 1U;
            return total;
        }
        ++state_->instrumentation.frame_builds;
        auto frame = source_frame;
        const auto& metadata = state_->hooks[hook.value];
        const auto range = state_->dispatch.hook_ranges[hook.value];
        const auto handlers =
            std::span<const State::Handler>{state_->dispatch.hook_handlers}.subspan(range.begin, range.count);
        for (const auto handler : handlers)
        {
            ++state_->instrumentation.handlers_visited;
            const auto result = state_->invoke(handler, frame, metadata.cardinality == ESystemHookCardinality::SINGLE);
            total.calls += result.calls;
            total.failures += result.failures;
            if (result.status != 0)
                total.status = result.status;
            if (metadata.cardinality == ESystemHookCardinality::SINGLE && result.calls != 0U)
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
        const bool is_missing_state = state_ == nullptr;
        const bool is_shutdown = !is_missing_state && state_->shut_down;
        const bool is_missing_event = !event;
        const bool is_out_of_range = !is_missing_state && event.value >= state_->events.size();
        const bool is_invalid_event = is_missing_state || is_shutdown || is_missing_event || is_out_of_range;
        if (is_invalid_event)
        {
            total.status = -1;
            total.failures = 1U;
            return total;
        }
        const auto& metadata = state_->events[event.value];
        if ((target == ecs::NullEntity) != (metadata.target == ESystemEventTarget::GLOBAL))
        {
            total.status = -1;
            total.failures = 1U;
            return total;
        }
        ++state_->instrumentation.frame_builds;
        auto frame = live_frame;
        std::span<const State::Handler> handlers;
        if (target == ecs::NullEntity)
        {
            handlers = state_->dispatch.global_event_handlers[event.value];
        }
        else
        {
            ++state_->instrumentation.entities_examined;
            handlers = state_->eventHandlersFor(target, event.value);
        }
        for (const auto handler : handlers)
        {
            ++state_->instrumentation.handlers_visited;
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

    std::span<const ScriptBindingFailure> ScriptBindingSession::failures() const noexcept
    {
        return state_ ? std::span<const ScriptBindingFailure>{state_->failures}
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

    const ScriptBindingInstrumentation& ScriptBindingSession::instrumentation() const noexcept
    {
        static const ScriptBindingInstrumentation empty;
        return state_ ? state_->instrumentation : empty;
    }
}
