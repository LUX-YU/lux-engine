#pragma once
/**
 * @file Script.hpp
 * @brief Pure-POD description of a script module (no payload, no I/O).
 *
 * `lux::rdesc::Script` is the engine's *resource description* of a script
 * module: everything an editor / runtime needs to know about the artefact
 * *without* loading code. Schema v2 (user ruling R1): the
 * kind-specific data lives in a `std::variant` BODY — each backend kind
 * carries exactly its own fields, and the codec writes a different block per
 * kind (a DLL writes its entry table; a Lua source writes its entry name).
 *
 * Common fields (name, dependencies, provenance) stay on the Script itself.
 * FlowForge-AOT artefacts are NativeModuleScript too (the AOT pipeline emits
 * a lux_script_abi shared library — identical loading path); provenance
 * (`compiler_id = "flowforge-aot"`, `source_id` = the FLOW_GRAPH uuid) plus
 * `state_layout_hash` discriminate them for editor UX.
 *
 * The compiled binary / source bytes are NOT stored here — they belong to
 * `lux::asset::ScriptAsset::payload()`. The `.luxasset` codec lives in the
 * asset layer (see `pinclude/.../ScriptDescriptionCodec.hpp`).
 *
 * The runtime contract for `NativeModuleScript` is fixed by
 * the Function Script ABI: once loaded, the library
 * exports `lux_script_get_module` and `ScriptHost` dispatches calls
 * through `lux_script_call_frame`.
 */

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace lux::rdesc
{
    /// Description of a single value slot (param / return).
    struct ScriptValueType
    {
        std::string   name;
        std::uint64_t type_id  = 0;
        std::uint32_t size     = 0;
        std::uint32_t align    = 0;
        std::uint8_t  kind     = 0u;
    };

    /// One entry in a native module's function table.
    struct ScriptFunction
    {
        std::string                  name;
        std::uint64_t                symbol_id = 0;
        std::vector<ScriptValueType> args;
        std::vector<ScriptValueType> returns;
        std::string                  native_symbol;
    };

    /// A resource the artefact depends on at runtime.
    struct ScriptDependency
    {
        std::string kind;
        std::string id;
    };

    /// Provenance — which compiler produced this artefact, with what inputs.
    struct ScriptProvenance
    {
        std::string compiler_id;
        std::string compiler_version;
        std::string source_id;
        std::string source_hash;
        std::string built_at;
    };

    // ── per-kind bodies (the variant alternatives) ──────────────────────────

    /// Lua source module: payload = UTF-8 source. The chunk returns a behavior
    /// table (OnCreate/OnUpdate/OnDestroy); `entry` names the behavior when a
    /// single source exports several (empty = the chunk's return value).
    struct LuaSourceScript
    {
        std::string entry;
    };

    /// Native shared-library module (hand-written C++ *or* FlowForge-AOT —
    /// provenance discriminates): payload = .dll/.so/.dylib bytes. `functions`
    /// is the exported entry table (the manifest); `abi_version` must match
    /// the host's lux_script_abi. `state_layout_hash` is the FlowForge
    /// instance-state recipe hash (0 = no per-instance state block).
    ///
    /// Instance-state recipe (Script Event Registry, ADR v2 step 1): the runtime
    /// allocates `state_size` bytes PER INSTANCE, copies `state_defaults`
    /// into the front (the rest zero-filled), and passes the block through
    /// `lux_script_call_frame.user_context` on every call — exactly the
    /// AotArtifact state block (see flowforge AOT). 0 = stateless module.
    struct NativeModuleScript
    {
        std::uint32_t               abi_version = 0;
        std::vector<ScriptFunction> functions;
        std::uint64_t               state_layout_hash = 0;
        std::uint32_t               state_size        = 0;
        std::vector<std::byte>      state_defaults;   ///< size() ≤ state_size
    };

    /// Reference to a statically-registered C++ ScriptBehavior
    /// (LUX_REGISTER_SCRIPT). The behavior's CODE lives in the binary; this
    /// asset is its manifest entry on disk — what makes code-defined behaviors
    /// first-class content (pickable / drag-droppable into
    /// ScriptComponent.script like any other script). Replaces the retired
    /// ScriptComponent.entry name path. A REAL file kind: authored via the
    /// editor's New Script ▸ C++, round-tripped by the codec.
    struct CppBehaviorScript
    {
        std::string behavior;   ///< registered behavior name (the manifest key)
    };

    /**
     * @brief Description of a script module (schema v2 — variant body).
     */
    class Script
    {
    public:
        /// Bump on any non-additive change to the description schema.
        /// v2: kind-specific fields moved into the variant `body` (R1).
        static constexpr std::uint32_t kSchemaVersion = 2u;

        /// Stable on-disk kind ids — EXPLICIT, never the variant index
        /// (reordering alternatives must not change the wire format). Values
        /// carried over from schema v1; gaps stay reserved (2 = LuaBytecode,
        /// 4 = PythonSource).
        enum class Kind : std::uint8_t
        {
            Unknown      = 0,
            LuaSource    = 1,
            NativeModule = 3,
            CppBehavior  = 6,   ///< registered-C++-behavior manifest entry
        };

        using Body = std::variant<std::monostate, LuaSourceScript,
                                  NativeModuleScript, CppBehaviorScript>;

        std::uint32_t                 schema_version = kSchemaVersion;
        std::string                   module_name;
        std::vector<ScriptDependency> dependencies;
        ScriptProvenance              provenance;
        Body                          body;   ///< per-kind data (monostate = Unknown)

        [[nodiscard]] Kind kind() const noexcept
        {
            if (std::holds_alternative<LuaSourceScript>(body))    return Kind::LuaSource;
            if (std::holds_alternative<NativeModuleScript>(body)) return Kind::NativeModule;
            if (std::holds_alternative<CppBehaviorScript>(body))  return Kind::CppBehavior;
            return Kind::Unknown;
        }
    };
}
