#pragma once
// ============================================================================
//  NativeModuleScriptBackend.hpp — the compiled-native-module backend
//  (lux::ecs; ScriptEventRegistry ADR v2).
//
//  Claims NativeModuleScript SCRIPT assets (payload = lux_script_abi shared-
//  library bytes — hand-written C++ plugins and FlowForge-AOT artefacts alike;
//  provenance discriminates, the loading path is identical). Wraps
//  modules/core/script's ScriptHost + NativeBackend (ABI validation, bind_host
//  import resolution, in-memory dll loading) into an entity-bound
//  IScriptBackend:
//
//    asset payload ──loadModuleFromMemory──▶ module (shared per asset)
//    instance      = per-entity STATE BLOCK (rdesc recipe: state_size bytes,
//                    state_defaults prefix, rest zero) + the module's resolved
//                    lifecycle entries; every call passes the block through
//                    call_frame.user_context.
//
//  Module lifetime = instance refcount: the last destroyed instance unloads
//  the dll (play-stop drops every instance → the module unloads — the ADR v2
//  ruling that stopping play means unloading). A changed payload under the
//  same cache_key reloads on the next instance creation (the play-boundary
//  hot-reload path, same contract as the Lua backend's source-hash recompile).
//
//  PIMPL: ScriptHost/NativeBackend stay in the .cpp — core::script is a
//  PRIVATE dep of the scripting target (consumers see no host headers).
// ============================================================================

#include <lux/engine/ecs/script/systems/ScriptBehavior.hpp>   // IScriptBackend / ScriptInstance
#include <lux/engine/function/visibility.h>

#include <functional>
#include <memory>
#include <string_view>

namespace lux::ecs
{
    class LUX_FUNCTION_PUBLIC NativeModuleScriptBackend final : public IScriptBackend
    {
    public:
        /// Engine-side import resolver for modules that export
        /// lux_script_bind_host (FlowForge-AOT artefacts import reflection
        /// trampolines + C-ABI host services). Same contract as
        /// lux::script::native_backend::HostSymbolResolver, redeclared so this
        /// header stays free of core::script includes: symbol name → address,
        /// nullptr = unknown. A module with ANY unresolved import is rejected
        /// at load — no half-bound module ever becomes callable.
        using HostSymbolResolver = std::function<void*(std::string_view)>;

        explicit NativeModuleScriptBackend(HostSymbolResolver resolver = {});
        ~NativeModuleScriptBackend() override;

        NativeModuleScriptBackend(const NativeModuleScriptBackend&)            = delete;
        NativeModuleScriptBackend& operator=(const NativeModuleScriptBackend&) = delete;

        [[nodiscard]] std::string_view kind() const override { return "native"; }

        /// Claims NativeModuleScript kinds; every other kind → empty (next
        /// backend). Loud-reject (stderr + empty) on: ABI version mismatch,
        /// empty payload, oversized state defaults, load/bind failure, or a
        /// manifest function missing from the loaded module's export table.
        /// Event binding (ADR v2 §3.2): each REGISTERED event resolves against
        /// the loaded export table by name; call-frame slot templates come
        /// from the LOADED signatures (the manifest is only a pre-check).
        [[nodiscard]] ScriptInstance
            createInstanceFromAsset(lux::meta::EntityHandle, World&,
                                    const lux::rdesc::Script&  desc,
                                    std::span<const std::byte> payload,
                                    std::string_view           cache_key) override;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;   // owns the ScriptHost; all host types confined to the .cpp
    };
} // namespace lux::ecs
