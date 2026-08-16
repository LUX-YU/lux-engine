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
#include <lux/engine/resource/asset/ScriptAsset.hpp>

#include <cstdio>
#include <span>
#include <utility>

namespace lux::ecs
{
    ScriptSystem::ScriptSystem(ScriptRegistry& registry, ScriptContext ctx)
        : registry_(registry), ctx_(ctx)
    {
    }

    // ── subscription index maintenance ──────────────────────────────────────

    void ScriptSystem::subscribe(lux::meta::entity_id entity,
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

    void ScriptSystem::unsubscribe(lux::meta::entity_id entity)
    {
        // Tombstone, don't erase: dispatch may be mid-iteration over one of
        // these lists (a script destroying a scripted entity lands here from
        // inside a dispatch batch). Tombstones die with the index at stop.
        for (auto& subs : by_event_)
            for (auto& s : subs)
                if (s.entity == entity)
                { s.state = nullptr; s.fn = nullptr; }
    }

    void ScriptSystem::faultSubscriber(lux::meta::EntityRegistry& registry,
                                       lux::meta::entity_id entity,
                                       ScriptEventId id)
    {
        if (auto* sc = registry.try_get<ScriptComponent>(entity))
            sc->enabled = false;   // faulting script disabled; engine survives
        unsubscribe(entity);
        reportScriptFault(entity, scriptEventRegistry().desc(id).name);
    }

    void ScriptSystem::onScriptComponentDestroyed(
        lux::meta::EntityRegistryBase&,
        entt::entity e)
    {
        unsubscribe(e);   // the slot (and its state) dies with the component
    }

    // ── dispatch (the module-facing surface) ────────────────────────────────

    void ScriptSystem::dispatch(lux::meta::EntityRegistry& registry,
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

    void ScriptSystem::dispatchTo(lux::meta::EntityRegistry& registry,
                                  lux::meta::entity_id entity,
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

    void ScriptSystem::onRuntimeStart(lux::meta::EntityRegistry& registry)
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
        lux::meta::EntityRegistry& registry)
    {
        registry.view<ScriptComponent>().each(
            [&](lux::meta::entity_id e, ScriptComponent& sc)
            {
                if (!sc.enabled || sc.instance.created)
                    return;

                // Assets are the ONLY attachment currency: SCRIPT assets
                // route by kind (Cpp manifest / Lua source / native module).
                // Authored FLOW_GRAPH assets are cooked to SCRIPT before a
                // Runtime product sees them.
                //
                // ⚠️ 定性(2026-08-04 同步收口批):这是 World tick 内仅存的
                // 同步 ensureAsset —— 「tick 中途注册资产」的唯一入口,所有
                // 宿主都走。此处不跨调用持任何 Data*(拿到立即消费),悬垂
                // 无虞;但它让脚本首帧带一次盘 IO 顿挫,应迁 requestLoad +
                // 就绪轮询(涉及脚本就绪时序,记档另立,本批不动)。
                ScriptInstance inst;
                if (!sc.script.is_nil() && ctx_.assets)
                {
                    if (auto loaded = ctx_.assets->ensureAsset(sc.script))
                    {
                        if (auto* sa = loaded.value()->as<lux::asset::ScriptAsset>();
                            sa && sa->data())
                        {
                            inst = registry_.createInstanceFromAsset(
                                lux::meta::EntityHandle{registry, e},
                                *ctx_.world,
                                *sa->data(), std::span<const std::byte>(sa->payload()),
                                uuids::to_string(sc.script));
                        }
                    }
                    else
                    {
                        std::fprintf(stderr,
                            "[ScriptSystem] entity %u: SCRIPT asset %s failed to load (err=%d)\n",
                            static_cast<unsigned>(e), uuids::to_string(sc.script).c_str(),
                            static_cast<int>(loaded.error()));
                    }
                }
                if (!inst)
                    return;
                sc.instance = std::move(inst);
                // 实例期驻留票:活着的脚本实例把源资产钉在账本上(流送闸门可见)。
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

    void ScriptSystem::stopRuntime(lux::meta::EntityRegistry& registry)
    {
        if (!running_)
            return;
        running_ = false;
        destroy_conn_.release();
        registry.view<ScriptComponent>().each(
            [&](lux::meta::entity_id e, ScriptComponent& sc)
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
