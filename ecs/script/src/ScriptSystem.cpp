#include <lux/engine/ecs/script/systems/ScriptSystem.hpp>

#include <lux/engine/ecs/script/detail/DirectDispatch.hpp>

#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/script/components/ScriptComponent.hpp>
#include <lux/engine/ecs/script/systems/ScriptRegistry.hpp>
#include <lux/engine/log/Log.hpp>
#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/resource/asset/script/ScriptAsset.hpp>

#include <exception>
#include <span>
#include <utility>

namespace lux::ecs
{
    namespace
    {
        lux_script_call_frame makeFrame(
            const ScriptEventDesc& event,
            void* const* arguments,
            lux_script_value_slot* slots,
            World* world
        ) noexcept
        {
            for (std::size_t i = 0; i < event.abi_params.size(); ++i)
            {
                const auto& type = event.abi_params[i];
                slots[i] = {};
                slots[i].kind = type.kind;
                slots[i].size = type.size;
                slots[i].type_id = type.type_id;
                slots[i].data = arguments ? arguments[i] : nullptr;
            }

            lux_script_call_frame frame{};
            frame.args = event.abi_params.empty() ? nullptr : slots;
            frame.arg_count = static_cast<std::uint32_t>(
                event.abi_params.size()
            );
            frame.world_context = world;
            return frame;
        }

        int invokeOne(
            BoundScriptCall call,
            const ScriptEventDesc& event,
            void* const* arguments,
            World* world
        ) noexcept
        {
            lux_script_value_slot slots[ScriptEventRegistry::kMaxParams]{};
            auto frame = makeFrame(event, arguments, slots, world);
            frame.user_context = call.context;
            return call.invoke(&frame);
        }

        void reportFailure(Entity entity, std::string_view event, int code)
        {
            lux::log::error(
                "ecs.script",
                "script event '{}' failed on entity {} with code {}",
                event,
                static_cast<std::uint32_t>(entity),
                code
            );
        }
    }

    ScriptSystem::ScriptSystem(
        ScriptRegistry& registry,
        ScriptContext ctx,
        std::vector<IScriptBackend*> backends
    )
        : registry_(registry)
        , ctx_(ctx)
        , backends_(std::move(backends))
    {}

    ScriptSystem::~ScriptSystem()
    {
        if (running_ && ctx_.world)
            stopRuntime(ctx_.world->registry());
    }

    void ScriptSystem::subscribe(Entity entity, const ScriptInstance& instance)
    {
        const auto events = instance.events();
        for (ScriptEventId id = 0; id < events.size(); ++id)
        {
            if (!events[id].invoke)
                continue;
            if (by_event_.size() <= id)
                by_event_.resize(id + 1);
            auto& subscribers = by_event_[id];
            subscribers.calls.push_back(events[id]);
            subscribers.owners.push_back(entity);
        }
    }

    void ScriptSystem::unsubscribe(Entity entity)
    {
        if (dispatching_)
        {
            lux::log::error(
                "ecs.script",
                "ScriptComponent structure changed during script dispatch"
            );
            std::terminate();
        }

        for (auto& subscribers : by_event_)
        {
            for (std::size_t i = 0; i < subscribers.owners.size();)
            {
                if (subscribers.owners[i] != entity)
                {
                    ++i;
                    continue;
                }
                subscribers.owners.erase(subscribers.owners.begin() + i);
                subscribers.calls.erase(subscribers.calls.begin() + i);
            }
        }
    }

    void ScriptSystem::onScriptComponentDestroyed(
        RegistryBase&,
        entt::entity entity
    )
    {
        unsubscribe(entity);
    }

    void ScriptSystem::dispatch(
        Registry& registry,
        ScriptEventId id,
        void* const* arguments
    )
    {
        if (id >= by_event_.size() || id >= scriptEventRegistry().count())
            return;

        const auto& event = scriptEventRegistry().desc(id);
        lux_script_value_slot slots[ScriptEventRegistry::kMaxParams]{};
        auto frame = makeFrame(event, arguments, slots, ctx_.world);
        auto& subscribers = by_event_[id];

        dispatching_ = true;
        std::size_t cursor = 0;
        while (cursor < subscribers.calls.size())
        {
            const auto outcome = detail::dispatchBoundCalls(
                subscribers.calls,
                frame,
                cursor
            );
            const std::size_t failed_index = outcome.failed_index;
            const int failed_code = outcome.result;

            if (failed_index == subscribers.calls.size())
                break;

            const Entity failed_entity = subscribers.owners[failed_index];
            dispatching_ = false;
            if (registry.all_of<ScriptComponent>(failed_entity))
            {
                registry.patch<ScriptComponent>(
                    failed_entity,
                    [](ScriptComponent& script) { script.enabled = false; }
                );
            }
            unsubscribe(failed_entity);
            reportFailure(failed_entity, event.name, failed_code);
            dispatching_ = true;
            cursor = failed_index;
        }
        dispatching_ = false;
    }

    void ScriptSystem::dispatchTo(
        Registry& registry,
        Entity entity,
        ScriptEventId id,
        void* const* arguments
    )
    {
        if (id >= scriptEventRegistry().count())
            return;
        auto* component = registry.try_get<ScriptComponent>(entity);
        if (!component || !component->enabled || !component->instance.created)
            return;
        const auto call = component->instance.inst.entry(id);
        if (!call.invoke)
            return;

        dispatching_ = true;
        const int result = invokeOne(
            call,
            scriptEventRegistry().desc(id),
            arguments,
            ctx_.world
        );
        dispatching_ = false;
        if (result != 0)
        {
            registry.patch<ScriptComponent>(
                entity,
                [](ScriptComponent& script) { script.enabled = false; }
            );
            unsubscribe(entity);
            reportFailure(entity, scriptEventRegistry().desc(id).name, result);
        }
    }

    void ScriptSystem::onRuntimeStart(Registry& registry)
    {
        if (running_)
            return;
        running_ = true;

        for (auto* backend : backends_)
            backend->beginFrame(ctx_);

        by_event_.assign(scriptEventRegistry().count(), {});
        destroy_conn_ = registry.on_destroy<ScriptComponent>()
            .connect<&ScriptSystem::onScriptComponentDestroyed>(*this);
        tryCreateInstances(registry);
    }

    void ScriptSystem::tryCreateInstances(Registry& registry)
    {
        registry.view<ScriptComponent>().each(
            [&](Entity entity, ScriptComponent& component)
            {
                if (!component.enabled || component.instance.created
                    || component.script.is_nil() || !ctx_.assets)
                    return;

                if (component.instance.ref.id() != component.script)
                    component.instance.ref = ctx_.assets->acquire(component.script);

                auto* asset = ctx_.assets->fetchAssetAs<lux::asset::ScriptAsset>(
                    component.script
                );
                if (!asset || !asset->data())
                    return;

                const auto self = EntityHandle{registry, entity};
                ScriptInstance instance = registry_.createCppInstanceFromAsset(
                    self,
                    *ctx_.world,
                    *asset->data()
                );
                if (!instance)
                {
                    for (auto* backend : backends_)
                    {
                        if (backend->kind() != asset->data()->kind())
                            continue;
                        instance = backend->createInstanceFromAsset(
                            self,
                            *ctx_.world,
                            *asset->data(),
                            std::span<const std::byte>(asset->payload()),
                            component.script,
                            ctx_.assets->contentRevision(component.script)
                        );
                        break;
                    }
                }
                if (!instance)
                    return;

                component.instance = std::move(instance);
                const auto create = component.instance.inst.entry(
                    ScriptEventRegistry::kOnCreate
                );
                const int result = create.invoke
                    ? invokeOne(
                        create,
                        scriptEventRegistry().desc(
                            ScriptEventRegistry::kOnCreate
                        ),
                        nullptr,
                        ctx_.world
                    )
                    : 0;
                if (result == 0)
                {
                    component.instance.created = true;
                    subscribe(entity, component.instance.inst);
                }
                else
                {
                    component.enabled = false;
                    component.instance.reset();
                    reportFailure(entity, "OnCreate", result);
                }
            }
        );
    }

    void ScriptSystem::update(const SystemUpdateContext& context)
    {
        auto& registry = context.registry();
        for (auto* backend : backends_)
            backend->beginFrame(ctx_);
        tryCreateInstances(registry);
        float dt = context.dt();
        void* arguments[1]{&dt};
        dispatch(registry, ScriptEventRegistry::kOnUpdate, arguments);
    }

    void ScriptSystem::onRemoved(const SystemRemovalContext& removal)
    {
        stopRuntime(removal.registry());
    }

    void ScriptSystem::stopRuntime(Registry& registry)
    {
        if (!running_)
            return;
        running_ = false;
        destroy_conn_.release();

        registry.view<ScriptComponent>().each(
            [&](Entity entity, ScriptComponent& component)
            {
                if (!component.instance)
                    return;
                const auto destroy = component.instance.inst.entry(
                    ScriptEventRegistry::kOnDestroy
                );
                if (destroy.invoke)
                {
                    const int result = invokeOne(
                        destroy,
                        scriptEventRegistry().desc(
                            ScriptEventRegistry::kOnDestroy
                        ),
                        nullptr,
                        ctx_.world
                    );
                    if (result != 0)
                        reportFailure(entity, "OnDestroy", result);
                }
                component.instance.reset();
            }
        );
        by_event_.clear();
    }
}
