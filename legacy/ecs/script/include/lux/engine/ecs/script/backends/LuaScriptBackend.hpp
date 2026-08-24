#pragma once
// ============================================================================
//  LuaScriptBackend.hpp — the Lua (LuaJIT + sol2) script backend (lux::ecs).
//
//  Grows the modules/core/script Lua stub into an ENTITY-BOUND behavior host:
//  a Lua script `return { OnCreate=…, OnUpdate=function(self,dt) … end, … }`
//  becomes a per-entity ScriptInstance whose callbacks receive `self` (the
//  entity, bound as a sol2 usertype). Lua execution uses protected calls; a
//  language error becomes a non-zero ABI result and ScriptSystem disables the
//  reporting component after leaving the successful hot loop.
//
//  PIMPL: sol/sol.hpp is heavy and pulls LuaJIT — kept entirely in the .cpp, so
//  this header (and every consumer of ecs/script) stays Lua-free. LuaJIT + sol2
//  are PRIVATE deps of the `scripting` target.
//
//  PHASE 2a: `self` exposes a MINIMAL hand-bound API (translate / x / y) that
//  touches Transform2D directly — a temporary, documented coupling to
//  ecs::components. Phase 2b replaces it with generic reflection access
//  (`self:get(component)`), which removes the coupling.
// ============================================================================

#include <lux/engine/ecs/script/systems/ScriptBehavior.hpp>   // IScriptBackend / ScriptInstance
#include <lux/engine/function/visibility.h>

#include <memory>
#include <string>
#include <string_view>

namespace lux::ecs
{
    class ComponentTypeCatalog;

    class LUX_FUNCTION_PUBLIC LuaScriptBackend final : public IScriptBackend
    {
    public:
        explicit LuaScriptBackend(const ComponentTypeCatalog& components);
        ~LuaScriptBackend() override;

        LuaScriptBackend(const LuaScriptBackend&)            = delete;
        LuaScriptBackend& operator=(const LuaScriptBackend&) = delete;

        [[nodiscard]] lux::rdesc::Script::Kind kind() const noexcept override
        {
            return lux::rdesc::Script::Kind::LuaSource;
        }

        /// Asset-routed creation: claims LuaSourceScript kinds. The
        /// payload (UTF-8 source) is compiled once for @p asset_id in the
        /// playback-session cache. Changed content becomes visible only after
        /// resetSession() and the next play start. Event entries resolve at
        /// bind by REGISTERED name from the module table (ADR v2 §3.2); events
        /// the table does not implement stay unbound. Other kinds → empty.
        [[nodiscard]] ScriptInstance
            createInstanceFromAsset(lux::ecs::EntityHandle, World&,
                                    const lux::rdesc::Script&  desc,
                                    std::span<const std::byte> payload,
                                    lux::asset::asset_id_t     asset_id,
                                    std::uint32_t content_revision) override;

        /// Capture this frame's input (for the Lua `Input` table).
        void beginFrame(const ScriptContext&) override;
        void resetSession() noexcept override;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;   // owns the sol::state; all sol types confined to the .cpp
    };
} // namespace lux::ecs
