#pragma once
// ============================================================================
//  ScriptEventRegistry.hpp — the process-domain script-event catalogue
//  (lux::ecs; ScriptEventRegistry ADR v2 §3.1).
//
//  ONE table of every event scripts can implement. Modules register their
//  events at STARTUP (same window as LUX_REGISTER_SCRIPT; runtime dynamic
//  registration is deliberately unsupported), and registration alone buys:
//    · a graph-palette Event node (FlowGraphPanel enumerates this table),
//    · a Lua module-table key (the Lua backend binds by event name),
//    · a C++ implementable method (built-ins now; generator shims in step 4),
//    · an AOT/native-module function-table entry (bound by name at load).
//
//  The three lifecycle events are ORDINARY entries this module registers
//  first (ids fixed 0/1/2) — no special dispatch status (ADR §3.1).
//
//  Same catalog pattern as ComponentTypeCatalog / SpawnRegistry: a
//  function-local-static singleton, dense ids in registration order,
//  session-stable. Ids index per-instance event tables and the ScriptSystem
//  subscription index directly.
// ============================================================================

#include <lux/engine/function/visibility.h>
#include <lux/engine/function/script/abi/lux_script_abi.h>
#include <lux/engine/meta/Meta.hpp>   // RefType (signature truth source)

#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lux::ecs
{
    /// Dense id in registration order — array index into per-instance event
    /// tables and the subscription index. Session-stable, never serialized.
    using ScriptEventId = std::uint32_t;
    inline constexpr ScriptEventId kInvalidScriptEvent = 0xFFFFFFFFu;

    /// One typed event parameter. `type` points into the reflection system's
    /// static type descriptions (ref_type_of_v) — the signature truth source;
    /// `name` feeds the graph palette pins and diagnostics.
    struct ScriptEventParam
    {
        const lux::meta::RefType* type = nullptr;
        std::string               name;
    };

    /// A registered event: name + typed payload, NO return value (fire-and-
    /// forget — ADR §5). Dispatch authority stays with the registering module;
    /// the registry is only the catalogue.
    struct ScriptEventDesc
    {
        std::string                   name;
        std::vector<ScriptEventParam> params;
        std::vector<lux_script_type_desc> abi_params;
    };

    class LUX_FUNCTION_PUBLIC ScriptEventRegistry
    {
    public:
        /// The built-in lifecycle events — registered by this module's own
        /// constructor, FIRST, so the ids are fixed constants.
        static constexpr ScriptEventId kOnCreate  = 0;   ///< ()
        static constexpr ScriptEventId kOnUpdate  = 1;   ///< (dt: float)
        static constexpr ScriptEventId kOnDestroy = 2;   ///< ()

        /// Per-event payload ceiling — keeps dispatch marshalling on the
        /// stack everywhere (native call-frame slots, Lua arg unpack).
        static constexpr std::size_t kMaxParams = 8;

        ScriptEventRegistry();
        ScriptEventRegistry(const ScriptEventRegistry&)            = delete;
        ScriptEventRegistry& operator=(const ScriptEventRegistry&) = delete;

        /// Register an event (STARTUP window only). Duplicate names return
        /// the existing id (idempotent — mirrors registerCppScript);
        /// > kMaxParams params is loudly rejected (kInvalidScriptEvent).
        ScriptEventId registerEvent(std::string_view name,
                                    std::vector<ScriptEventParam> params);

        [[nodiscard]] ScriptEventId find(std::string_view name) const;
        [[nodiscard]] const ScriptEventDesc& desc(ScriptEventId id) const;
        [[nodiscard]] std::size_t count() const noexcept;

    private:
        std::deque<ScriptEventDesc>                          entries_;   // deque: desc() refs stay stable
        std::unordered_map<std::string_view, ScriptEventId>  by_name_;   // keys view into entries_
    };

    /// Process-wide catalogue (function-local static — safe init order).
    [[nodiscard]] LUX_FUNCTION_PUBLIC ScriptEventRegistry& scriptEventRegistry();
} // namespace lux::ecs
