#include <lux/engine/simulation/systems/ScriptSystem.hpp>
#include <lux/engine/simulation/detail/DenseEntityHandlerStorage.hpp>

#include <lux/cxx/container/SlotMap.hpp>

#include <entt/signal/sigh.hpp>

#include <algorithm>
#include <array>
#include <exception>
#include <limits>
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

        enum class EMountState : std::uint8_t
        {
            INACTIVE,
            CONSTRUCTING,
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

        [[nodiscard]] bool sameType(
            const lux::rdesc::ScriptValueType &script_type,
            const lux::semantic::Type &endpoint_type) noexcept
        {
            return script_type.type_id == endpoint_type.type_id &&
                script_type.canonical_name == endpoint_type.canonical_name &&
                script_type.pass == endpoint_type.pass;
        }

        [[nodiscard]] bool sameHookSignature(
            const lux::rdesc::ScriptFunction &function,
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

        [[nodiscard]] bool sameEventSignature(
            const lux::rdesc::ScriptFunction &function,
            const lux::semantic::Type &payload) noexcept
        {
            return function.returns.empty() && function.args.size() == 1U &&
                sameType(function.args.front(), payload);
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
            const ScriptMountDescription *authored{};
            std::size_t binding_first{};
            std::size_t binding_count{};
            std::size_t method_first{};
            std::size_t method_count{};
            ScriptInstanceScope scope;
            ScriptBehavior behavior;
            ResolvedScriptArtifact artifact;
            const ScriptBackendDescriptor *backend{};
            ScriptBackendInstance backend_instance;
            ecs::Entity entity{ecs::NullEntity};
            EMountState state{EMountState::INACTIVE};
            bool active_counted{};
            bool retirement_queued{};
        };

        struct HookBucket final
        {
            State *owner{};
            const ScriptHookEndpointDescriptor *endpoint{};
            EndpointConnectionToken token;
            HandlerStorage handlers;
            std::size_t handler_capacity{};
        };

        struct EventBucket final
        {
            State *owner{};
            const ScriptEventEndpointDescriptor *endpoint{};
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

        const SimulationDescription *simulation{};
        const ScriptSystemDescription *description{};
        ecs::Registry *registry{};
        std::size_t failure_capacity{};
        ScriptArtifactResolver artifacts;
        WorldObjectResolver world;
        ScriptHostApi host;
        std::array<ScriptBackendDescriptor, kBackendKindCount> backends;
        std::vector<ScriptHookEndpointDescriptor> hook_endpoints;
        std::vector<ScriptEventEndpointDescriptor> event_endpoints;
        std::unordered_map<EndpointKey, std::uint32_t, EndpointKeyHash> hook_endpoint_index;
        std::unordered_map<EndpointKey, std::uint32_t, EndpointKeyHash> event_endpoint_index;
        std::vector<RuntimeMount> mounts;
        std::vector<RuntimeBinding> bindings;
        std::vector<PreparedMethod> methods;
        std::vector<HookBucket> hooks;
        std::vector<EventBucket> events;
        std::vector<ScriptSystemFailure> failures;
        std::vector<std::uint32_t> retirement_queue;
        SparseMountQueue dirty_current;
        SparseMountQueue dirty_processing;
        entt::connection constructed;
        entt::connection updated;
        entt::connection destroyed;
        std::size_t active_mount_count{};
        bool suppress_attachment_signal{};
        EPrepareState prepare_state{EPrepareState::CREATED};

        [[nodiscard]] static constexpr EndpointConnectionToken hookToken(HandlerKey key) noexcept
        {
            return {key.index, key.gen};
        }

        [[nodiscard]] static constexpr HandlerKey hookKey(EndpointConnectionToken token) noexcept
        {
            return {token.slot, token.generation};
        }

        [[nodiscard]] const ScriptBackendDescriptor *backend(lux::rdesc::Script::Kind kind) const noexcept
        {
            const auto index = backendIndex(kind);
            if (index >= backends.size() || backends[index].kind != kind)
                return nullptr;
            return std::addressof(backends[index]);
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
                for (const auto &mount : authored_mounts)
                    binding_capacity += mount.enabled ? mount.bindings.size() : 0U;

                mounts.clear();
                mounts.resize(authored_mounts.size());
                bindings.clear();
                bindings.reserve(binding_capacity);
                methods.clear();
                methods.reserve(binding_capacity);
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
                    const auto &authored = authored_mounts[mount_slot];
                    auto &mount = mounts[mount_slot];
                    mount.authored = std::addressof(authored);
                    mount.binding_first = bindings.size();
                    mount.method_first = methods.size();
                    if (!authored.enabled)
                        continue;

                    std::unordered_map<lux::script::ScriptSymbolId, std::uint32_t> method_index;
                    method_index.reserve(authored.bindings.size());

                    for (const auto &binding : authored.bindings)
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
                            methods.push_back({binding.symbol, {}});
                            method_index.emplace(binding.symbol, runtime.method_slot);
                            ++mount.method_count;
                        }
                        if (const auto *target = std::get_if<HookScriptTarget>(&binding.target))
                        {
                            const auto endpoint = findHookEndpoint(*target);
                            if (!endpoint)
                                return lux::cxx::unexpected(EScriptSystemError::ENDPOINT_NOT_FOUND);

                            runtime.kind = EBindingKind::HOOK;
                            runtime.bucket_slot = *endpoint;
                            ++hooks[runtime.bucket_slot].handler_capacity;
                        }
                        else
                        {
                            const auto event_target = std::get<EventScriptTarget>(binding.target);
                            const auto endpoint = findEventEndpoint(event_target);
                            if (!endpoint)
                                return lux::cxx::unexpected(EScriptSystemError::ENDPOINT_NOT_FOUND);

                            runtime.kind = EBindingKind::EVENT;
                            runtime.bucket_slot = *endpoint;
                            ++events[runtime.bucket_slot].handler_capacity;
                        }
                        bindings.push_back(runtime);
                        ++mount.binding_count;
                    }
                }

                for (auto &bucket : hooks)
                    bucket.handlers.reserve(bucket.handler_capacity);
                for (auto &bucket : events)
                {
                    const auto prepared = bucket.handlers.prepare(bucket.handler_capacity);
                    if (prepared == EEndpointMutationError::ALLOCATION_FAILURE)
                        return lux::cxx::unexpected(EScriptSystemError::ALLOCATION_FAILURE);
                }

                dirty_current.prepare(mounts.size());
                dirty_processing.prepare(mounts.size());
                retirement_queue.clear();
                retirement_queue.reserve(mounts.size());
                return {};
            }
            catch (const std::bad_alloc &)
            {
                return lux::cxx::unexpected(EScriptSystemError::ALLOCATION_FAILURE);
            }
        }

        void recordFailure(
            EScriptSystemError error,
            RuntimeMount &mount,
            lux::script::ScriptSymbolId symbol = 0U,
            std::int32_t status = 0) noexcept
        {
            if (failures.size() < failure_capacity)
                failures.push_back({error, mount.authored->id, symbol, status});
        }

        void deactivate(RuntimeMount &mount) noexcept
        {
            if (!mount.active_counted)
                return;
            mount.active_counted = false;
            --active_mount_count;
        }

        void queueRetirement(std::uint32_t mount_slot) noexcept
        {
            auto &mount = mounts[mount_slot];
            if (mount.retirement_queued)
                return;
            if (retirement_queue.size() >= retirement_queue.capacity())
                std::terminate();

            mount.retirement_queued = true;
            retirement_queue.push_back(mount_slot);
        }

        void invoke(Handler &handler, lux_script_call_frame &frame) noexcept
        {
            auto &mount = mounts[handler.mount_slot];
            if (mount.state != EMountState::ACTIVE)
                return;

            auto &method = methods[handler.method_slot];
            frame.user_context = method.call.context;
            const auto status = method.call.invoke(&frame);
            if (status == 0)
                return;

            mount.state = EMountState::FAULTED;
            deactivate(mount);
            queueRetirement(handler.mount_slot);
            recordFailure(EScriptSystemError::INVOCATION_FAILURE, mount, method.symbol, status);
        }

        static void invokeHookLane(void *context, lux_script_call_frame &frame) noexcept
        {
            auto &bucket = *static_cast<HookBucket *>(context);
            for (auto &handler : bucket.handlers.values())
                bucket.owner->invoke(handler, frame);
        }

        static void dispatchEvent(void *context, ecs::Entity entity, lux_script_call_frame &frame) noexcept
        {
            auto &bucket = *static_cast<EventBucket *>(context);
            const auto invoke = [&bucket, &frame](Handler& handler) noexcept
            {
                bucket.owner->invoke(handler, frame);
            };
            if (bucket.endpoint->route == EEventRoute::SIMULATION_BROADCAST)
            {
                bucket.handlers.forEachAll(invoke);
                return;
            }
            bucket.handlers.forEachTarget(entity, invoke);
        }

        [[nodiscard]] lux::cxx::expected<EndpointConnectionToken, EScriptSystemError> addEventHandler(
            EventBucket &bucket,
            std::uint32_t mount_slot,
            std::uint32_t method_slot,
            ecs::Entity target) noexcept
        {
            const bool is_broadcast = bucket.endpoint->route == EEventRoute::SIMULATION_BROADCAST;
            if (!is_broadcast && target == ecs::NullEntity)
                return lux::cxx::unexpected(EScriptSystemError::SCOPE_MISMATCH);

            const auto inserted = bucket.handlers.connect(
                target,
                Handler{mount_slot, method_slot},
                is_broadcast
            );
            if (inserted)
                return inserted.token;
            const auto error = inserted.error == EEndpointMutationError::CAPACITY_EXCEEDED
                ? EScriptSystemError::CAPACITY_EXCEEDED
                : EScriptSystemError::ALLOCATION_FAILURE;
            return lux::cxx::unexpected(error);
        }

        void removeEventHandler(EventBucket &bucket, EndpointConnectionToken token) noexcept
        {
            static_cast<void>(bucket.handlers.disconnect(token));
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptSystemError> bindMount(std::uint32_t mount_slot) noexcept
        {
            auto &mount = mounts[mount_slot];
            const auto binding_end = mount.binding_first + mount.binding_count;
            for (std::size_t binding_slot{mount.binding_first}; binding_slot < binding_end; ++binding_slot)
            {
                auto &binding = bindings[binding_slot];
                if (binding.kind == EBindingKind::HOOK)
                {
                    auto &bucket = hooks[binding.bucket_slot];
                    if (bucket.handlers.size() >= bucket.handler_capacity)
                        return lux::cxx::unexpected(EScriptSystemError::CAPACITY_EXCEEDED);

                    const auto inserted = bucket.handlers.tryEmplace(
                        Handler{mount_slot, binding.method_slot}
                    );
                    if (!inserted)
                        return lux::cxx::unexpected(EScriptSystemError::ALLOCATION_FAILURE);
                    binding.registration = hookToken(*inserted);
                    continue;
                }

                auto &bucket = events[binding.bucket_slot];
                const auto inserted = addEventHandler(bucket, mount_slot, binding.method_slot, mount.entity);
                if (!inserted)
                    return lux::cxx::unexpected(inserted.error());
                binding.registration = *inserted;
            }
            return {};
        }

        void removeMountBindings(RuntimeMount &mount) noexcept
        {
            const auto binding_end = mount.binding_first + mount.binding_count;
            for (std::size_t binding_slot{mount.binding_first}; binding_slot < binding_end; ++binding_slot)
            {
                auto &binding = bindings[binding_slot];
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

        [[nodiscard]] lux::cxx::expected<void, EScriptSystemError> projectAttachment(
            std::uint32_t mount_slot,
            ecs::Entity entity) noexcept
        {
            if (registry->all_of<detail::ScriptAttachment>(entity))
            {
                const auto &attachment = registry->get<detail::ScriptAttachment>(entity);
                return attachment.mount_slot == mount_slot
                    ? lux::cxx::expected<void, EScriptSystemError>{}
                    : lux::cxx::expected<void, EScriptSystemError>(
                        lux::cxx::unexpected(EScriptSystemError::SCOPE_MISMATCH)
                    );
            }

            suppress_attachment_signal = true;
            try
            {
                registry->emplace<detail::ScriptAttachment>(entity, mount_slot);
            }
            catch (const std::bad_alloc &)
            {
                suppress_attachment_signal = false;
                return lux::cxx::unexpected(EScriptSystemError::ALLOCATION_FAILURE);
            }
            suppress_attachment_signal = false;
            return {};
        }

        void removeOwnedAttachment(std::uint32_t mount_slot, ecs::Entity entity) noexcept
        {
            if (!ownsAttachment(mount_slot, entity))
                return;
            suppress_attachment_signal = true;
            registry->remove<detail::ScriptAttachment>(entity);
            suppress_attachment_signal = false;
        }

        void resetMountRuntime(RuntimeMount &mount) noexcept
        {
            mount.scope = SimulationScriptScope{};
            mount.behavior = {};
            mount.artifact = {};
            mount.backend = nullptr;
            mount.backend_instance = {};
            mount.entity = ecs::NullEntity;
            mount.retirement_queued = false;
        }

        void releaseMount(std::uint32_t mount_slot, EMountState final_state, bool remove_attachment) noexcept
        {
            auto &mount = mounts[mount_slot];
            removeMountBindings(mount);
            if (remove_attachment)
                removeOwnedAttachment(mount_slot, mount.entity);

            if (mount.backend != nullptr && mount.backend_instance)
            {
                const auto method_end = mount.method_first + mount.method_count;
                for (std::size_t index{method_end}; index > mount.method_first; --index)
                {
                    auto &method = methods[index - 1U];
                    if (!method.call)
                        continue;
                    mount.backend->releaseMethod(
                        mount.backend->context,
                        mount.backend_instance,
                        method.call
                    );
                    method.call = {};
                }
                mount.backend->destroyInstance(mount.backend->context, mount.backend_instance);
            }
            if (mount.artifact.lease != nullptr && mount.artifact.release != nullptr)
                mount.artifact.release(mount.artifact.lease);

            deactivate(mount);
            resetMountRuntime(mount);
            mount.state = final_state;
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptSystemError> prepareMount(
            std::uint32_t mount_slot,
            std::optional<ecs::Entity> forced_entity = std::nullopt) noexcept
        {
            auto &mount = mounts[mount_slot];
            if (!mount.authored->enabled || mount.state == EMountState::FAULTED)
                return {};
            if (mount.state == EMountState::ACTIVE)
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
                const auto &object = std::get<EntityScriptMount>(mount.authored->scope).object;
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
            if (mount.artifact.artifact == nullptr ||
                !lux::rdesc::validScriptDescription(mount.artifact.artifact->description()))
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

            const ScriptInstanceCreateContext create_context{
                mount.authored->asset,
                mount.scope,
                std::addressof(mount.behavior)
            };
            const auto created = mount.backend->createInstance(
                mount.backend->context,
                create_context,
                *mount.artifact.artifact,
                mount.backend_instance
            );
            if (created != EScriptBackendResult::SUCCESS)
            {
                const auto error = backendError(created);
                releaseMount(mount_slot, EMountState::INACTIVE, false);
                return lux::cxx::unexpected(error);
            }

            const auto method_end = mount.method_first + mount.method_count;
            for (std::size_t method_slot{mount.method_first}; method_slot < method_end; ++method_slot)
            {
                auto &method = methods[method_slot];
                const auto* function = mount.artifact.artifact->findExport(method.symbol);
                if (function == nullptr)
                {
                    releaseMount(mount_slot, EMountState::INACTIVE, false);
                    return lux::cxx::unexpected(EScriptSystemError::SYMBOL_NOT_FOUND);
                }

                const auto prepared_method = mount.backend->prepareMethod(
                    mount.backend->context,
                    mount.backend_instance,
                    *function,
                    method.call
                );
                if (prepared_method != EScriptBackendResult::SUCCESS || !method.call)
                {
                    const auto error = backendError(prepared_method);
                    releaseMount(mount_slot, EMountState::INACTIVE, false);
                    return lux::cxx::unexpected(error);
                }
            }

            const auto binding_end = mount.binding_first + mount.binding_count;
            for (std::size_t binding_slot{mount.binding_first}; binding_slot < binding_end; ++binding_slot)
            {
                const auto &binding = bindings[binding_slot];
                const auto &authored = mount.authored->bindings[binding_slot - mount.binding_first];
                const auto* function = mount.artifact.artifact->findExport(methods[binding.method_slot].symbol);
                const bool is_hook = binding.kind == EBindingKind::HOOK;
                const bool is_signature_valid = is_hook
                    ? sameHookSignature(*function, hooks[binding.bucket_slot].endpoint->signature)
                    : sameEventSignature(*function, events[binding.bucket_slot].endpoint->payload_type);
                if (!is_signature_valid)
                {
                    releaseMount(mount_slot, EMountState::INACTIVE, false);
                    return lux::cxx::unexpected(EScriptSystemError::SIGNATURE_MISMATCH);
                }

                if (!is_hook)
                {
                    const auto &endpoint = *events[binding.bucket_slot].endpoint;
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

            const auto bound = bindMount(mount_slot);
            if (!bound)
            {
                const auto error = bound.error();
                releaseMount(mount_slot, EMountState::INACTIVE, false);
                return lux::cxx::unexpected(error);
            }

            if (mount.entity != ecs::NullEntity)
            {
                const auto projected = projectAttachment(mount_slot, mount.entity);
                if (!projected)
                {
                    const auto error = projected.error();
                    releaseMount(mount_slot, EMountState::INACTIVE, false);
                    return lux::cxx::unexpected(error);
                }
            }

            mount.state = EMountState::ACTIVE;
            mount.active_counted = true;
            ++active_mount_count;
            return {};
        }

        void queueDirty(std::uint32_t mount_slot) noexcept
        {
            if (!dirty_current.insert(mount_slot))
                std::terminate();
        }

        void handleAttachmentSignal(ecs::Registry &source, ecs::Entity entity, bool destroying) noexcept
        {
            if (suppress_attachment_signal || !source.all_of<detail::ScriptAttachment>(entity))
                return;

            const auto mount_slot = source.get<detail::ScriptAttachment>(entity).mount_slot;
            if (mount_slot >= mounts.size())
                return;
            auto &mount = mounts[mount_slot];
            if (mount.entity != entity)
                return;

            if (destroying && mount.state == EMountState::ACTIVE)
            {
                mount.state = EMountState::RETIRING;
                deactivate(mount);
            }
            queueDirty(mount_slot);
        }

        void onAttachmentConstructed(ecs::Registry &source, ecs::Entity entity) noexcept
        {
            handleAttachmentSignal(source, entity, false);
        }

        void onAttachmentUpdated(ecs::Registry &source, ecs::Entity entity) noexcept
        {
            handleAttachmentSignal(source, entity, false);
        }

        void onAttachmentDestroyed(ecs::Registry &source, ecs::Entity entity) noexcept
        {
            handleAttachmentSignal(source, entity, true);
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptSystemError> connectEndpoints() noexcept
        {
            for (auto &bucket : hooks)
            {
                if (bucket.handler_capacity == 0U)
                    continue;
                const auto connected = bucket.endpoint->connect(
                    bucket.endpoint->context,
                    std::addressof(bucket),
                    &State::invokeHookLane
                );
                if (!connected)
                    return lux::cxx::unexpected(EScriptSystemError::ENDPOINT_CONNECTION_FAILURE);
                bucket.token = connected.token;
            }
            for (auto &bucket : events)
            {
                if (bucket.handler_capacity == 0U)
                    continue;
                const auto connected = bucket.endpoint->connect(
                    bucket.endpoint->context,
                    std::addressof(bucket),
                    &State::dispatchEvent
                );
                if (!connected)
                    return lux::cxx::unexpected(EScriptSystemError::ENDPOINT_CONNECTION_FAILURE);
                bucket.token = connected.token;
            }
            return {};
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptSystemError> disconnectEndpoints() noexcept
        {
            bool busy{};
            for (auto &bucket : hooks)
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
            for (auto &bucket : events)
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
            return busy
                ? lux::cxx::expected<void, EScriptSystemError>(
                    lux::cxx::unexpected(EScriptSystemError::ENDPOINT_BUSY)
                )
                : lux::cxx::expected<void, EScriptSystemError>{};
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
            for (std::size_t index{mounts.size()}; index > 0U; --index)
                releaseMount(static_cast<std::uint32_t>(index - 1U), EMountState::INACTIVE, true);
            active_mount_count = 0U;
            dirty_current.clear();
            dirty_processing.clear();
            retirement_queue.clear();
            prepare_state = EPrepareState::CREATED;
            return {};
        }
    };

    lux::cxx::expected<ScriptSystem, EScriptSystemError> ScriptSystem::create(
        const SimulationDescription &simulation,
        const ScriptSystemDescription &description,
        ecs::Registry &registry,
        std::size_t failure_capacity,
        ScriptArtifactResolver artifacts,
        WorldObjectResolver world,
        std::span<const ScriptBackendDescriptor> backends,
        std::span<const ScriptHookEndpointDescriptor> hooks,
        std::span<const ScriptEventEndpointDescriptor> events,
        ScriptHostApi host) noexcept
    {
        if (failure_capacity == 0U || artifacts.resolve == nullptr)
            return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);

        std::array<ScriptBackendDescriptor, kBackendKindCount> backend_table{};
        for (const auto &backend : backends)
        {
            const auto index = backendIndex(backend.kind);
            const bool is_invalid_kind = backend.kind == lux::rdesc::Script::Kind::UNKNOWN ||
                index >= backend_table.size();
            const bool is_invalid_functions = backend.createInstance == nullptr ||
                backend.prepareMethod == nullptr ||
                backend.releaseMethod == nullptr ||
                backend.destroyInstance == nullptr;
            if (is_invalid_kind || is_invalid_functions)
                return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);
            if (backend_table[index].kind != lux::rdesc::Script::Kind::UNKNOWN)
                return lux::cxx::unexpected(EScriptSystemError::DUPLICATE_BACKEND_KIND);
            backend_table[index] = backend;
        }

        try
        {
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
                const bool is_invalid_signature = !described ||
                    described.parameterCount() != hooks[index].signature.parameters.size() ||
                    !hooks[index].signature.returns.empty();
                if (is_invalid_identity || is_invalid_functions || is_invalid_signature)
                    return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);

                for (std::size_t parameter{}; parameter < described.parameterCount(); ++parameter)
                {
                    if (described.parameterAt(parameter) != hooks[index].signature.parameters[parameter])
                        return lux::cxx::unexpected(EScriptSystemError::SIGNATURE_MISMATCH);
                }
                const auto inserted = hook_endpoint_index.emplace(
                    EndpointKey{hooks[index].system.value, hooks[index].hook.value},
                    static_cast<std::uint32_t>(index)
                );
                if (!inserted.second)
                    return lux::cxx::unexpected(EScriptSystemError::DUPLICATE_ENDPOINT);
            }

            for (std::size_t index{}; index < events.size(); ++index)
            {
                if (index >= std::numeric_limits<std::uint32_t>::max())
                    return lux::cxx::unexpected(EScriptSystemError::CAPACITY_EXCEEDED);

                const auto described = simulation.findEvent(events[index].system, events[index].event);
                const bool is_invalid_identity = !events[index].system.valid() || !events[index].event.valid();
                const bool is_invalid_functions = events[index].connect == nullptr ||
                    events[index].disconnect == nullptr;
                const bool is_invalid_signature = !described ||
                    described.route() != events[index].route ||
                    described.payloadType() != events[index].payload_type.type_id ||
                    described.payloadSchemaName() != events[index].payload_type.canonical_name ||
                    events[index].payload_type.pass != lux::semantic::EValuePass::CONST_REF;
                if (is_invalid_identity || is_invalid_functions || is_invalid_signature)
                    return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);

                const auto inserted = event_endpoint_index.emplace(
                    EndpointKey{events[index].system.value, events[index].event.value},
                    static_cast<std::uint32_t>(index)
                );
                if (!inserted.second)
                    return lux::cxx::unexpected(EScriptSystemError::DUPLICATE_ENDPOINT);
            }

            auto state = std::make_unique<State>();
            state->simulation = std::addressof(simulation);
            state->description = std::addressof(description);
            state->registry = std::addressof(registry);
            state->artifacts = artifacts;
            state->world = world;
            state->host = host;
            state->backends = backend_table;
            state->hook_endpoints.assign(hooks.begin(), hooks.end());
            state->event_endpoints.assign(events.begin(), events.end());
            state->hook_endpoint_index = std::move(hook_endpoint_index);
            state->event_endpoint_index = std::move(event_endpoint_index);
            state->failure_capacity = failure_capacity;
            state->failures.reserve(failure_capacity);
            return ScriptSystem(std::move(state));
        }
        catch (const std::bad_alloc &)
        {
            return lux::cxx::unexpected(EScriptSystemError::ALLOCATION_FAILURE);
        }
    }

    ScriptSystem::ScriptSystem(std::unique_ptr<State> state) noexcept : state_(std::move(state))
    {
    }

    ScriptSystem::ScriptSystem(ScriptSystem &&) noexcept = default;
    ScriptSystem &ScriptSystem::operator=(ScriptSystem &&) noexcept = default;

    ScriptSystem::~ScriptSystem() noexcept
    {
        if (state_ && state_->prepare_state != EPrepareState::SHUT_DOWN && !shutdown())
            std::terminate();
    }

    lux::cxx::expected<void, EScriptSystemError> ScriptSystem::prepare() noexcept
    {
        if (!state_ || state_->prepare_state == EPrepareState::SHUT_DOWN)
            return lux::cxx::unexpected(EScriptSystemError::SHUT_DOWN);
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

        state_->constructed = state_->registry
            ->on_construct<detail::ScriptAttachment>()
            .template connect<&State::onAttachmentConstructed>(*state_);
        state_->updated = state_->registry
            ->on_update<detail::ScriptAttachment>()
            .template connect<&State::onAttachmentUpdated>(*state_);
        state_->destroyed = state_->registry
            ->on_destroy<detail::ScriptAttachment>()
            .template connect<&State::onAttachmentDestroyed>(*state_);

        for (std::size_t mount_slot{}; mount_slot < state_->mounts.size(); ++mount_slot)
        {
            const auto prepared_mount = state_->prepareMount(static_cast<std::uint32_t>(mount_slot));
            if (!prepared_mount)
            {
                const auto rolled_back = state_->rollbackPrepare();
                return rolled_back ? prepared_mount : rolled_back;
            }
        }

        const auto connected = state_->connectEndpoints();
        if (!connected)
        {
            const auto rolled_back = state_->rollbackPrepare();
            return rolled_back ? connected : rolled_back;
        }

        state_->dirty_current.clear();
        state_->dirty_processing.clear();
        state_->retirement_queue.clear();
        state_->prepare_state = EPrepareState::PREPARED;
        return {};
    }

    lux::cxx::expected<void, EScriptSystemError> ScriptSystem::flushMutations() noexcept
    {
        if (!state_ || state_->prepare_state == EPrepareState::SHUT_DOWN)
            return lux::cxx::unexpected(EScriptSystemError::SHUT_DOWN);
        if (state_->prepare_state == EPrepareState::ROLLBACK_PENDING)
            return lux::cxx::unexpected(EScriptSystemError::ENDPOINT_BUSY);
        if (state_->prepare_state != EPrepareState::PREPARED)
            return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);

        for (const auto mount_slot : state_->retirement_queue)
        {
            auto &mount = state_->mounts[mount_slot];
            if (mount.state == EMountState::FAULTED)
                state_->releaseMount(mount_slot, EMountState::FAULTED, true);
        }
        state_->retirement_queue.clear();

        state_->dirty_processing.clear();
        std::swap(state_->dirty_current, state_->dirty_processing);
        std::optional<EScriptSystemError> first_error;
        for (const auto mount_slot : state_->dirty_processing.values)
        {
            auto &mount = state_->mounts[mount_slot];
            if (mount.state == EMountState::FAULTED)
                continue;

            const bool attachment_matches = state_->ownsAttachment(mount_slot, mount.entity);
            if (mount.state == EMountState::ACTIVE && attachment_matches)
                continue;

            if (mount.state != EMountState::INACTIVE)
                state_->releaseMount(mount_slot, EMountState::INACTIVE, attachment_matches);

            const auto attached = state_->prepareMount(mount_slot);
            if (attached)
                continue;

            const auto error = attached.error();
            if (!first_error)
                first_error = error;
            if (error == EScriptSystemError::WORLD_OBJECT_NOT_RESOLVED)
            {
                state_->queueDirty(mount_slot);
                continue;
            }

            mount.state = EMountState::FAULTED;
            state_->recordFailure(error, mount);
        }
        state_->dirty_processing.clear();

        return first_error
            ? lux::cxx::expected<void, EScriptSystemError>(lux::cxx::unexpected(*first_error))
            : lux::cxx::expected<void, EScriptSystemError>{};
    }

    lux::cxx::expected<void, EScriptSystemError> ScriptSystem::shutdown() noexcept
    {
        if (!state_ || state_->prepare_state == EPrepareState::SHUT_DOWN)
            return {};

        const auto disconnected = state_->disconnectEndpoints();
        if (!disconnected)
            return disconnected;

        state_->releaseSignals();
        for (std::size_t index{state_->mounts.size()}; index > 0U; --index)
            state_->releaseMount(static_cast<std::uint32_t>(index - 1U), EMountState::INACTIVE, true);

        state_->dirty_current.clear();
        state_->dirty_processing.clear();
        state_->retirement_queue.clear();
        state_->prepare_state = EPrepareState::SHUT_DOWN;
        return {};
    }

    std::size_t ScriptSystem::activeInstanceCount() const noexcept
    {
        return state_ ? state_->active_mount_count : 0U;
    }

    std::span<const ScriptSystemFailure> ScriptSystem::failures() const noexcept
    {
        return state_ ? std::span<const ScriptSystemFailure>(state_->failures)
                      : std::span<const ScriptSystemFailure>{};
    }
}
