#pragma once
// ============================================================================
//  ScriptSystem.hpp — the single driver of all script instances (lux::ecs).
//
//  ScriptEventRegistry ADR v2 §3.3: the system owns the SUBSCRIPTION INDEX — a
//  per-event dense list of implementers built at instance bind. Dispatch is
//  zero-filter: modules call dispatch()/dispatchTo() and only entities whose
//  instance actually bound that event are touched; no ScriptComponent view
//  walk, no virtual call, no string (the hot path is table[id] direct calls).
//
//  Play-scoped:
//    onRuntimeStart — create instances, fire OnCreate, build the index
//    update         — dispatch(kOnUpdate) every frame (ISystem; play only)
//    onRemoved      — fire OnDestroy, drop instances, clear the index
//
//  Crash guard (§6b + ADR §3.5 refinement): dispatch runs each event's subscriber
//  list under ONE guard with a live cursor — a fault identifies the culprit
//  EXACTLY (no bisect, no re-runs), disables it, and resumes with the rest.
//
//  ScriptContext carries per-run services to scripts (threaded to backends
//  each frame AND at instance creation via ScriptRegistry::beginFrame).
//
//  ── 定位(统一事件系统批F 评估):**这不是事件系统,事件走 lux::events**。
//  dispatch/dispatchTo 是「引擎 → 脚本世界」的定向调用面(订阅索引 = 它的
//  实现,不是总线的重复;设计稿 §6-10):
//    · 生命周期三派发点(kOnCreate/Update/Destroy)保留直调 —— onUpdate
//      每实体每帧、本系统自产自销、帧内顺序敏感,走总线是纯开销(性能豁免
//      条款,设计稿批F);
//    · 物理碰撞链(Physics2DSystem::setCollisionSink → 播放会话装配层接线
//      → dispatchTo)已是「模块回调缝 + 装配层接线」的裁决③合规形;经泵
//      会把脚本响应拖出 fixed-step 同帧相位,且单消费者零扇出收益,评估后
//      不迁(批D viewport 同款判据);
//    · 将来某个**总线事件**要进脚本世界时,在装配层写一条
//      `bus.subscribe<E>(pump, [scripts](e){ 打包 void* args → dispatch })`
//      即成 —— 不需要预建泛型桥。
// ============================================================================

#include <lux/engine/ecs/script/systems/ScriptEventRegistry.hpp>   // ScriptEventId
#include <lux/engine/ecs/script/systems/ScriptBehavior.hpp>        // ScriptEventFn
#include <lux/engine/ecs/systems/ISystem.hpp>
#include <lux/engine/function/visibility.h>

#include <vector>

namespace lux::input { class ActionMapper; class InputActionRegistry; }
namespace lux::asset { class AssetManager; }

namespace lux::ecs
{
    class World;
    class ScriptRegistry;
    class ScriptInstance;

    /// Per-run services handed to scripts. Pointers may be null (e.g. headless
    /// with no input). Threaded to backends via ScriptRegistry::beginFrame, so
    /// `ISystem::update`'s signature is unchanged.
    struct ScriptContext
    {
        World*                                 world   = nullptr;
        const lux::input::ActionMapper*        input   = nullptr;   // phase 3 (B)
        const lux::input::InputActionRegistry* actions = nullptr;   // name → ActionId
        lux::asset::AssetManager*              assets  = nullptr;   // SCRIPT asset resolution
    };

    class LUX_FUNCTION_PUBLIC ScriptSystem final : public lux::ecs::ISystem
    {
    public:
        ScriptSystem(ScriptRegistry& registry, ScriptContext ctx);

        // Non-copyable (holds a reference member; dllexport-safe).
        ScriptSystem(const ScriptSystem&)            = delete;
        ScriptSystem& operator=(const ScriptSystem&) = delete;

        /// Entering play: instantiate every enabled ScriptComponent, fire
        /// OnCreate, and build the subscription index.
        void onRuntimeStart(lux::meta::EntityRegistry& registry);

        /// Per-frame (play only): dispatch(kOnUpdate).
        void update(const lux::ecs::SystemUpdateContext& ctx) override;

        /// Dynamic removal is the sole play-session closing path. Keeping the
        /// inverse operation inside the opt-in system makes direct
        /// `Schedule::removeSystem` just as safe as the SceneRuntime path.
        void onRemoved(
            const lux::ecs::SystemRemovalContext& removal
        ) override;

        [[nodiscard]] bool supportsDynamicRemoval() const noexcept override
        {
            return true;
        }

        // ── the module-facing dispatch surface (ADR §3.3) ──────────────────
        /// Fire @p id at EVERY implementer. @p args follows the packed
        /// convention (args[i] → param i storage, valid for the call only).
        void dispatch(lux::meta::EntityRegistry& registry, ScriptEventId id,
                      void* const* args);

        /// Fire @p id at ONE entity's instance (directed events — collision
        /// pairs, messages). No-op when the entity does not implement it.
        void dispatchTo(lux::meta::EntityRegistry& registry,
                        lux::meta::entity_id entity, ScriptEventId id,
                        void* const* args);

    private:
        /// One implementer of one event: the bound entry + its state, plus
        /// the entity for fault attribution. state==nullptr = tombstone
        /// (unsubscribed mid-session; compacted on the next runtime start).
        struct Subscriber
        {
            lux::meta::entity_id entity{};
            void*                state = nullptr;
            ScriptEventFn        fn    = nullptr;
        };

        void subscribe(lux::meta::entity_id entity, const ScriptInstance& inst);
        void unsubscribe(lux::meta::entity_id entity);
        void faultSubscriber(lux::meta::EntityRegistry& registry,
                             lux::meta::entity_id entity, ScriptEventId id);
        void tryCreateInstances(lux::meta::EntityRegistry& registry);
        void onScriptComponentDestroyed(
            lux::meta::EntityRegistryBase&,
            entt::entity);
        void stopRuntime(lux::meta::EntityRegistry& registry);

        ScriptRegistry& registry_;
        ScriptContext   ctx_;

        std::vector<std::vector<Subscriber>> by_event_;   // [ScriptEventId] → implementers
        entt::scoped_connection              destroy_conn_{};
        bool                                 running_{false};
    };
} // namespace lux::ecs
