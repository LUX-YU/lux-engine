#pragma once
// ============================================================================
//  ScriptBehavior.hpp — the per-entity script contracts (lux::ecs;
//  ScriptEventRegistry ADR v2 §3.2: an instance is a bind-time-resolved event
//  entry table, IScriptInstance is retired).
//
//    ScriptEventFn   — the ONE dispatch entry convention every backend
//                      satisfies: packed args (the existing lux_script_abi /
//                      reflection-trampoline convention), bool = clean run.
//    ScriptInstance  — a live per-entity script instance: opaque backend
//                      state + destructor + a dense [ScriptEventId] → entry
//                      table resolved ONCE at bind (instance creation).
//                      Hot-path dispatch is table[id](state, id, args):
//                      no virtual call, no string, empty slot = skip.
//    ScriptBehavior  — the C++ AUTHORING base (user face unchanged, ADR §4):
//                      inherit and override onCreate/onUpdate/onDestroy. The
//                      built-in shims call through these virtuals for now;
//                      step 4's generator emits direct per-type shims.
//    IScriptBackend  — resolves a script ASSET into a ScriptInstance.
//
//  STATE DISCIPLINE (ADR §A note): transient runtime state → instance members
//  (play-scoped, reset on OnCreate); authored/persistent/tunable state →
//  COMPONENTS. Instances are created on play-start and destroyed on
//  play-stop, so member state can never leak across play sessions.
// ============================================================================

#include <lux/engine/function/visibility.h>
#include <lux/engine/ecs/Registry.hpp>
#include <lux/engine/description/Script.hpp>   // rdesc::Script — asset-routed creation
#include <lux/engine/ecs/script/systems/ScriptEventRegistry.hpp>   // ScriptEventId

#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace lux::ecs
{
    class World;
    struct ScriptContext;

    /// The one dispatch-entry convention (ADR §3.2, refined: the dispatcher
    /// passes the event id it already holds, so one backend thunk can serve
    /// every slot; per-event shims simply ignore it).
    ///   state — the instance's opaque backend state
    ///   args  — packed payload, args[i] points at param i's storage; valid
    ///           ONLY for the duration of the call (scripts must not keep it)
    ///   return false = the script REPORTED failure (dispatcher disables it)
    using ScriptEventFn = bool (*)(void* state, ScriptEventId id,
                                   void* const* args);

    /// A live per-entity script instance (replaces the IScriptInstance
    /// inheritance). Move-only RAII: destruction runs `drop` on the state.
    class ScriptInstance
    {
    public:
        ScriptInstance() = default;
        ScriptInstance(void* state, void (*drop)(void*)) noexcept
            : state_(state), drop_(drop) {}

        ScriptInstance(const ScriptInstance&)            = delete;
        ScriptInstance& operator=(const ScriptInstance&) = delete;
        ScriptInstance(ScriptInstance&& o) noexcept
            : state_(std::exchange(o.state_, nullptr))
            , drop_(std::exchange(o.drop_, nullptr))
            , events_(std::move(o.events_)) {}
        ScriptInstance& operator=(ScriptInstance&& o) noexcept
        {
            if (this != &o)
            {
                release();
                state_  = std::exchange(o.state_, nullptr);
                drop_   = std::exchange(o.drop_, nullptr);
                events_ = std::move(o.events_);
            }
            return *this;
        }
        ~ScriptInstance() { release(); }

        /// Bind an event entry (instance-creation time only).
        void bind(ScriptEventId id, ScriptEventFn fn)
        {
            if (id == kInvalidScriptEvent || fn == nullptr) return;
            if (events_.size() <= id) events_.resize(id + 1, nullptr);
            events_[id] = fn;
        }

        /// The entry for @p id — nullptr = this instance does not implement it.
        [[nodiscard]] ScriptEventFn entry(ScriptEventId id) const noexcept
        {
            return id < events_.size() ? events_[id] : nullptr;
        }

        [[nodiscard]] std::span<const ScriptEventFn> events() const noexcept
        { return events_; }
        [[nodiscard]] void* state() const noexcept { return state_; }
        [[nodiscard]] explicit operator bool() const noexcept { return state_ != nullptr; }

    private:
        void release() noexcept
        {
            if (state_ && drop_) drop_(state_);
            state_ = nullptr;
            drop_  = nullptr;
        }

        void*                      state_ = nullptr;
        void                     (*drop_)(void*) = nullptr;
        std::vector<ScriptEventFn> events_;   // [ScriptEventId] → entry; empty slot = not implemented
    };

    /// C++ authoring base: `class PlayerBehavior : public ScriptBehavior { ... }`.
    /// Named XxxBehavior — a behavior attached to an entity, NOT the entity.
    /// The virtuals remain the USER FACE (ADR §4); the built-in shims route
    /// through them (one virtual hop — the cheapest link in §1's table) until
    /// step 4's generator emits direct per-type shims.
    class LUX_FUNCTION_PUBLIC ScriptBehavior
    {
    public:
        virtual ~ScriptBehavior() = default;
        virtual void onCreate()         {}
        virtual void onUpdate(float dt) { (void)dt; }
        virtual void onDestroy()        {}

    protected:
        template<typename C> C&   getComponent()       { return self_.get<C>(); }
        template<typename C> bool hasComponent() const { return self_.all_of<C>(); }

        /// **改组件走这里。**
        ///
        /// `getComponent<C>()` 返回的是裸引用,直接写它**不发 `on_update` 信号**。
        /// 而引擎里的消费者是**变更驱动**的 —— 变换系统的快闸门、渲染抽取节点
        /// 都靠信号发现「这个实体要重算」。裸写的后果是那次修改**永远不被消费**:
        /// 物体不动、精灵不更新,而且**零报错**。
        ///
        /// 所以脚本要改组件就用这个:
        ///
        ///     patchComponent<Transform2DComponent>([dt](auto& t) {
        ///         t.rotation += 1.5f * dt;
        ///     });
        ///
        /// (`getComponent` 保留给**读**。这是 C++ 侧对 Bevy `Mut<T>` 的对应:
        ///  那边由借用检查强制经过打戳的包装,这边由 API 形状引导。)
        template<typename C, typename Fn>
        void patchComponent(Fn&& fn) { self_.patch<C>(std::forward<Fn>(fn)); }
        [[nodiscard]] lux::ecs::EntityHandle self() const noexcept
        {
            return self_;
        }
        [[nodiscard]] lux::ecs::Entity entity() const noexcept { return self_.entity(); }
        [[nodiscard]] World& world() const noexcept { return *world_; }

    private:
        friend class ScriptRegistry;   // injects self_/world_ before onCreate
        lux::ecs::EntityHandle self_{};
        World*       world_{nullptr};
    };

    /// Resolves a script ASSET into a per-entity instance. One backend per
    /// Kind. (A-6: assets are the only attachment currency.)
    class IScriptBackend
    {
    public:
        virtual ~IScriptBackend() = default;
        [[nodiscard]] virtual std::string_view kind() const = 0;   // "lua"/"native"/"flowforge"

        /// Called once per frame BEFORE dispatch, so a backend can capture the
        /// current per-frame services (input, …) for its scripts. Default no-op.
        virtual void beginFrame(const ScriptContext&) {}

        /// Asset-routed creation: the backend inspects the asset's
        /// DESCRIPTION and claims kinds it owns (Lua backend → LuaSourceScript;
        /// native loader → NativeModuleScript). @p payload is the asset's raw
        /// body (Lua source text / DLL bytes); @p cache_key is a stable
        /// per-asset key (the uuid string) for compile caching / hot reload.
        /// Return an EMPTY instance for "not mine" — the registry tries the
        /// next backend. Event entries are resolved HERE, once (ADR §3.2).
        virtual ScriptInstance
            createInstanceFromAsset(lux::ecs::EntityHandle, World&,
                                    const lux::rdesc::Script& /*desc*/,
                                    std::span<const std::byte> /*payload*/,
                                    std::string_view /*cache_key*/)
        { return {}; }
    };
} // namespace lux::ecs
