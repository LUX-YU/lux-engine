#pragma once
// ============================================================================
//  NativeModuleScriptBackend.hpp — the compiled-native-module backend
//  (lux::ecs; ScriptEventRegistry ADR v2).
//
//  Claims NativeModuleScript SCRIPT assets (payload = lux_script_abi shared-
//  library bytes — hand-written C++ plugins and FlowForge-AOT artefacts alike;
//  provenance discriminates, the loading path is identical). The concrete
//  function/script NativeModule performs load-time ABI validation and host
//  import resolution; this cold-path owner binds descriptors into ECS calls:
//
//    asset payload ──loadModuleFromMemory──▶ module (shared per asset)
//    instance      = per-entity STATE BLOCK (rdesc recipe: state_size bytes,
//                    state_defaults prefix, rest zero) + the module's resolved
//                    lifecycle entries; every call passes the block through
//                    call_frame.user_context.
//
//  Module lifetime = playback session. The first version observed for an asset
//  id remains the session snapshot even when its last instance disappears.
//  resetSession() unloads modules only after every instance has been destroyed.
//
//  PIMPL keeps NativeModule and platform loader details out of the ECS API.
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

        [[nodiscard]] lux::rdesc::Script::Kind kind() const noexcept override
        {
            return lux::rdesc::Script::Kind::NativeModule;
        }

        /// Claims NativeModuleScript kinds; every other kind → empty (next
        /// backend). Returns empty on ABI version mismatch,
        /// empty payload, oversized state defaults, load/bind failure, or a
        /// manifest function missing from the loaded module's export table.
        /// Event binding (ADR v2 §3.2): each REGISTERED event resolves against
        /// the loaded export table by name; call-frame slot templates come
        /// from the LOADED signatures (the manifest is only a pre-check).
        [[nodiscard]] ScriptInstance
            createInstanceFromAsset(lux::ecs::EntityHandle, World&,
                                    const lux::rdesc::Script&  desc,
                                    std::span<const std::byte> payload,
                                    lux::asset::asset_id_t     asset_id,
                                    std::uint32_t content_revision) override;

        void resetSession() noexcept override;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::ecs
