#pragma once
// ============================================================================
//  ScriptComponent.hpp — the ONE script component (lux::ecs, scripting ADR v3.3).
//  A behavior asset attached to an entity + its play-scoped live instance.
//
//  Reflected (LUX_COMPONENT) so it survives a scene save/load — which the editor
//  Edit/Play snapshot relies on — and appears in the Inspector's Add Component
//  menu. Only `script`/`enabled` are LUX_MEMBER (authored data); the per-entity
//  live instance is runtime-only (LUX_NO_MEMBER — the meta generator skips it).
//
//  COPY SEMANTICS: copying a script attachment copies the AUTHORED
//  fields; the live instance is play-scoped state and never duplicates. That
//  is expressed by the slot type below (copy = empty slot) so the component
//  itself stays RULE-OF-ZERO — the meta generator reflects every public method
//  of a LUX_COMPONENT class, and a user-declared operator= (returns a
//  reference) breaks its invokable codegen.
// ============================================================================

#include <lux/engine/ecs/script/systems/ScriptBehavior.hpp>   // ScriptInstance (event-entry-table form, ADR v2)
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/resource/asset/Asset.hpp>      // asset_id_t — the SCRIPT asset reference
#include <lux/engine/resource/asset/AssetRef.hpp>   // 实例期驻留票(未注解类型,生成器不见)

#include <string>
#include <utility>

namespace lux::ecs
{
    /// Play-scoped live-instance slot: the bound ScriptInstance (event entry
    /// table, ADR v2 §3.2) plus the "onCreate already fired" flag, with
    /// COPY = EMPTY (a copied attachment starts un-instantiated, exactly like
    /// a fresh attach). NOT annotated — the reflection generator never sees
    /// its methods.
    struct ScriptInstanceSlot
    {
        /// SCRIPT 资产的驻留票 —— 实例活着期间源资产不得被流送驱逐
        /// (此前脚本消费者对账本全瞎)。与实例同生命期:play-stop/fault 的
        /// reset() 一并还票。
        lux::asset::AssetRef ref;
        ScriptInstance inst;
        bool           created = false;   ///< onCreate already fired?

        ScriptInstanceSlot() = default;
        ScriptInstanceSlot(const ScriptInstanceSlot&) noexcept {}
        ScriptInstanceSlot& operator=(const ScriptInstanceSlot&) noexcept
        { reset(); return *this; }
        ScriptInstanceSlot(ScriptInstanceSlot&&) noexcept            = default;
        ScriptInstanceSlot& operator=(ScriptInstanceSlot&&) noexcept = default;

        ScriptInstanceSlot& operator=(ScriptInstance i) noexcept
        { inst = std::move(i); return *this; }

        [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(inst); }
        void reset() noexcept { inst = ScriptInstance{}; created = false; ref.reset(); }
    };

    struct LUX_COMPONENT() ScriptComponent
    {
        /// The SCRIPT asset (drag one from the AssetBrowser / pick via
        /// the Inspector). Kind routing (Lua source / native module) happens at
        /// play-start from the asset's description.
        LUX_MEMBER(display_name=Script, asset_type=script, tooltip=Cooked SCRIPT asset (.lua / native module / registered C++ behavior))
        lux::asset::asset_id_t script{};

        // (A-6: the `entry` name path is GONE — every script attaches through
        // the asset above. C++ behaviors are content too: a CppBehaviorScript
        // SCRIPT asset names the registered behavior — authored via the
        // browser's New Script ▸ C++. Multi-entry modules, when they land,
        // select inside the ASSET's description, not on the component.)

        LUX_MEMBER(display_name=Enabled)
        bool        enabled = true;  ///< false ⇒ skipped (also set after an ABI-reported failure)

        // ── runtime-only (never serialized / reflected): per-entity instance ──
        ScriptInstanceSlot LUX_NO_MEMBER() instance;   ///< play-scoped; created on play-start
    };
} // namespace lux::ecs
