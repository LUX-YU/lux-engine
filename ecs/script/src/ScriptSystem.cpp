// ============================================================================
//  ScriptSystem.cpp — instance lifecycle + the subscription-index dispatcher
//  (see the Script Event Registry, ADR v2 §3.3): dispatch touches ONLY
//  implementers, through the instances' bound entry tables — no virtual, no
//  string on the hot path.
//
//  Guard granularity (ADR §3.5, refined): one guard per subscriber BATCH with a
//  live cursor. A hardware fault (or a script-reported failure) identifies
//  the culprit exactly — no bisect, no double-runs — which gets disabled;
//  dispatch resumes with the remaining subscribers. The fault-free hot path
//  pays ONE guard per event, not one per call.
// ============================================================================

#include <lux/engine/ecs/script/systems/ScriptSystem.hpp>

#include <lux/engine/ecs/script/components/ScriptComponent.hpp>
#include <lux/engine/ecs/script/systems/ScriptCrashGuard.hpp>
#include <lux/engine/ecs/script/systems/ScriptRegistry.hpp>
#include <lux/engine/ecs/World.hpp>

#include <lux/engine/resource/asset/AssetManager.hpp>   // SCRIPT asset resolution
#include <lux/engine/resource/asset/script/ScriptAsset.hpp>

#include <span>
#include <utility>

namespace lux::ecs
{
    ScriptSystem::ScriptSystem(ScriptRegistry& registry, ScriptContext ctx)
        : registry_(registry), ctx_(ctx)
    {
    }

    // ── subscription index maintenance ──────────────────────────────────────

    void ScriptSystem::subscribe(lux::ecs::Entity entity,
                                 const ScriptInstance& inst)
    {
        const auto events = inst.events();
        for (ScriptEventId id = 0; id < events.size(); ++id)
        {
            if (!events[id]) continue;
            if (by_event_.size() <= id) by_event_.resize(id + 1);
            by_event_[id].push_back(Subscriber{ entity, inst.state(), events[id] });
        }
    }

    void ScriptSystem::unsubscribe(lux::ecs::Entity entity)
    {
        // Tombstone, don't erase: dispatch may be mid-iteration over one of
        // these lists (a script destroying a scripted entity lands here from
        // inside a dispatch batch). Tombstones die with the index at stop.
        for (auto& subs : by_event_)
            for (auto& s : subs)
                if (s.entity == entity)
                { s.state = nullptr; s.fn = nullptr; }
    }

    void ScriptSystem::faultSubscriber(lux::ecs::Registry& registry,
                                       lux::ecs::Entity entity,
                                       ScriptEventId id)
    {
        if (auto* sc = registry.try_get<ScriptComponent>(entity))
            sc->enabled = false;   // faulting script disabled; engine survives
        unsubscribe(entity);
        reportScriptFault(entity, scriptEventRegistry().desc(id).name);
    }

    void ScriptSystem::onScriptComponentDestroyed(
        lux::ecs::RegistryBase&,
        entt::entity e)
    {
        unsubscribe(e);   // the slot (and its state) dies with the component
    }

    // ── dispatch (the module-facing surface) ────────────────────────────────

    void ScriptSystem::dispatch(lux::ecs::Registry& registry,
                                ScriptEventId id, void* const* args)
    {
        if (id >= by_event_.size()) return;
        auto& subs = by_event_[id];

        std::size_t cursor = 0;
        while (cursor < subs.size())
        {
            // One guard per batch; `cursor` is live inside, so a fault (SEH /
            // exception / script-reported false) points at the exact culprit.
            (void)guardedScriptCall([&]
            {
                for (; cursor < subs.size(); ++cursor)
                {
                    const Subscriber& s = subs[cursor];
                    if (!s.fn) continue;                     // tombstone
                    if (!s.fn(s.state, id, args))
                        return;                              // script-reported failure
                }
            });
            if (cursor >= subs.size())
                break;                                       // clean batch end
            faultSubscriber(registry, subs[cursor].entity, id);
            ++cursor;                                        // isolate & resume
        }
    }

    void ScriptSystem::dispatchTo(lux::ecs::Registry& registry,
                                  lux::ecs::Entity entity,
                                  ScriptEventId id, void* const* args)
    {
        if (id >= by_event_.size()) return;
        for (const Subscriber& s : by_event_[id])
        {
            if (s.entity != entity || !s.fn) continue;
            // Directed events are low-frequency — per-call guard is fine.
            bool reported_ok = true;
            const bool clean = guardedScriptCall(
                [&] { reported_ok = s.fn(s.state, id, args); });
            if (!clean || !reported_ok)
                faultSubscriber(registry, entity, id);
            return;
        }
    }

    // ── play lifecycle ──────────────────────────────────────────────────────

    void ScriptSystem::onRuntimeStart(lux::ecs::Registry& registry)
    {
        if (running_)
            return;
        running_ = true;

        // Hand the backends this session's services BEFORE any instance is
        // created — creation itself needs them (the flowforge backend loads
        // the FLOW_GRAPH asset through ctx.assets).
        registry_.beginFrame(ctx_);

        by_event_.assign(scriptEventRegistry().count(), {});
        destroy_conn_ = registry.on_destroy<ScriptComponent>()
                            .connect<&ScriptSystem::onScriptComponentDestroyed>(*this);

        tryCreateInstances(registry);
    }

    void ScriptSystem::tryCreateInstances(
        lux::ecs::Registry& registry)
    {
        registry.view<ScriptComponent>().each(
            [&](lux::ecs::Entity e, ScriptComponent& sc)
            {
                if (!sc.enabled || sc.instance.created)
                    return;

                // Assets are the ONLY attachment currency: SCRIPT assets
                // route by kind (Cpp manifest / Lua source / native module).
                // Authored FLOW_GRAPH assets are cooked to SCRIPT before a
                // Runtime product sees them.
                //
                ScriptInstance inst;
                if (!sc.script.is_nil() && ctx_.assets)
                {
                    if (sc.instance.ref.id() != sc.script)
                        sc.instance.ref = ctx_.assets->acquire(sc.script);
                    if (auto* asset = ctx_.assets->fetchAssetAs<
                            lux::asset::ScriptAsset>(sc.script);
                        asset && asset->data())
                    {
                        inst = registry_.createInstanceFromAsset(
                            lux::ecs::EntityHandle{registry, e},
                            *ctx_.world,
                            *asset->data(),
                            std::span<const std::byte>(asset->payload()),
                            uuids::to_string(sc.script));
                    }
                }
                if (!inst)
                    return;
                sc.instance = std::move(inst);
                // 实例期驻留票:活着的脚本实例把源资产钉在账本上(流送闸门可见)。
                if (sc.instance.ref.id() != sc.script)
                    sc.instance.ref = ctx_.assets->acquire(sc.script);

                // OnCreate fires DIRECTLY (creation order), before the entity
                // joins the index — an instance that faults in OnCreate never
                // becomes a subscriber.
                bool reported_ok = true;
                bool clean       = true;
                if (const auto fn = sc.instance.inst.entry(ScriptEventRegistry::kOnCreate))
                {
                    clean = guardedScriptCall(
                        [&] { reported_ok = fn(sc.instance.inst.state(),
                                               ScriptEventRegistry::kOnCreate, nullptr); });
                }
                if (clean && reported_ok)
                {
                    sc.instance.created = true;
                    subscribe(e, sc.instance.inst);
                }
                else
                {
                    sc.enabled = false;   // faulted in onCreate — disable, keep engine alive
                    sc.instance.reset();
                    reportScriptFault(e, "OnCreate");
                }
            });
    }

    void ScriptSystem::update(const lux::ecs::SystemUpdateContext& ctx)
    {
        auto& registry = ctx.registry();
        float dt = ctx.dt();          // 非 const:下面要把它的地址塞进 void* 实参槽
        registry_.beginFrame(ctx_);   // hand this frame's input/services to the backends
        // Enabled components without a live instance are in STARTING. Asset
        // reads and FlowForge AOT compilation may finish independently of the
        // render frame; retrying here performs only main-thread adoption and
        // never blocks on a worker or GPU result.
        tryCreateInstances(registry);
        void* args[1] = { &dt };
        dispatch(registry, ScriptEventRegistry::kOnUpdate, args);
    }

    void ScriptSystem::onRemoved(
        const lux::ecs::SystemRemovalContext& removal
    )
    {
        stopRuntime(removal.registry());
    }

    void ScriptSystem::stopRuntime(lux::ecs::Registry& registry)
    {
        if (!running_)
            return;
        running_ = false;
        destroy_conn_.release();
        registry.view<ScriptComponent>().each(
            [&](lux::ecs::Entity e, ScriptComponent& sc)
            {
                if (!sc.instance)
                    return;
                // OnDestroy is best-effort; ignore its result (we drop it anyway).
                if (const auto fn = sc.instance.inst.entry(ScriptEventRegistry::kOnDestroy))
                {
                    bool reported_ok = true;
                    if (!guardedScriptCall(
                            [&] { reported_ok = fn(sc.instance.inst.state(),
                                                   ScriptEventRegistry::kOnDestroy,
                                                   nullptr); })
                        || !reported_ok)
                        reportScriptFault(e, "OnDestroy");
                }
                sc.instance.reset();   // clears the live instance AND the created flag
            });
        by_event_.clear();
    }
} // namespace lux::ecs
