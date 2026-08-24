#pragma once
// ============================================================================
//  ScriptBehavior.hpp — the per-entity script contracts (lux::ecs;
//  ScriptEventRegistry ADR v2 §3.2: an instance is a bind-time-resolved event
//  entry table, IScriptInstance is retired).
//
//    BoundScriptCall — final ABI entry + per-instance context. All language
//                      selection and signature checks happen before binding.
//    ScriptInstance  — a live per-entity script instance: opaque backend
//                      state + destructor + a dense [ScriptEventId] → entry
//                      table resolved ONCE at bind (instance creation).
//                      Hot-path dispatch is one final ABI function call:
//                      no virtual call, no string, empty slot = skip.
//    ScriptBehavior  — non-polymorphic C++ authoring context base. Optional
//                      exact noexcept members are detected by registration.
//    IScriptBackend  — cold-path session binder. Never entered by dispatch.
//
//  STATE DISCIPLINE (ADR §A note): transient runtime state → instance members
//  (play-scoped, reset on OnCreate); authored/persistent/tunable state →
//  COMPONENTS. Instances are created on play-start and destroyed on
//  play-stop, so member state can never leak across play sessions.
// ============================================================================

#include <lux/engine/function/visibility.h>
#include <lux/engine/function/script/abi/lux_script_abi.h>
#include <lux/engine/ecs/Registry.hpp>
#include <lux/engine/description/Script.hpp>   // rdesc::Script — asset-routed creation
#include <lux/engine/ecs/script/systems/ScriptEventRegistry.hpp>   // ScriptEventId
#include <lux/engine/resource/asset/Asset.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace lux::ecs
{
    class World;
    struct ScriptContext;

    struct BoundScriptCall final
    {
        lux_script_invoke_fn invoke{nullptr};
        void*                context{nullptr};

        friend bool operator==(
            const BoundScriptCall&,
            const BoundScriptCall&
        ) = default;
    };

    static_assert(sizeof(BoundScriptCall) == 2 * sizeof(void*));
    static_assert(std::is_trivially_copyable_v<BoundScriptCall>);

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
        void bind(ScriptEventId id, BoundScriptCall call)
        {
            if (id == kInvalidScriptEvent || call.invoke == nullptr) return;
            if (events_.size() <= id) events_.resize(id + 1);
            events_[id] = call;
        }

        /// The entry for @p id — nullptr = this instance does not implement it.
        [[nodiscard]] BoundScriptCall entry(ScriptEventId id) const noexcept
        {
            return id < events_.size() ? events_[id] : BoundScriptCall{};
        }

        [[nodiscard]] std::span<const BoundScriptCall> events() const noexcept
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
        std::vector<BoundScriptCall> events_;
    };

    /// C++ authoring base: `class PlayerBehavior : public ScriptBehavior { ... }`.
    /// Named XxxBehavior — a behavior attached to an entity, NOT the entity.
    /// Lifecycle methods are optional concrete members on the derived type;
    /// registration emits exact noexcept ABI thunks with no vtable.
    class LUX_FUNCTION_PUBLIC ScriptBehavior
    {
    protected:
        template<typename C> const C& getComponent() const { return self_.get<C>(); }
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
        template<class T> friend struct ScriptBehaviorAccess;
        lux::ecs::EntityHandle self_{};
        World*       world_{nullptr};
    };

    /// Resolves a script ASSET into a per-entity instance. One backend per
    /// Kind. (A-6: assets are the only attachment currency.)
    class IScriptBackend
    {
    public:
        virtual ~IScriptBackend() = default;
        [[nodiscard]] virtual lux::rdesc::Script::Kind kind() const noexcept = 0;

        /// Called once per frame BEFORE dispatch, so a backend can capture the
        /// current per-frame services (input, …) for its scripts. Default no-op.
        virtual void beginFrame(const ScriptContext&) {}

        /// Drop every derived-code cache after all instances have gone.
        virtual void resetSession() noexcept = 0;

        /// Asset-routed creation: the backend inspects the asset's
        /// DESCRIPTION and claims kinds it owns (Lua backend → LuaSourceScript;
        /// native loader → NativeModuleScript). @p payload is the asset's raw
        /// body (Lua source text / DLL bytes); @p asset_id keys the playback
        /// session cache. Event entries are resolved and validated HERE once.
        virtual ScriptInstance
            createInstanceFromAsset(lux::ecs::EntityHandle, World&,
                                    const lux::rdesc::Script& /*desc*/,
                                    std::span<const std::byte> /*payload*/,
                                    lux::asset::asset_id_t /*asset_id*/,
                                    std::uint32_t /*content_revision*/)
        { return {}; }
    };
} // namespace lux::ecs
