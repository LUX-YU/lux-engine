// ============================================================================
//  NativeModuleScriptBackend.cpp — ScriptHost/NativeBackend live here and
//  NOWHERE else in ecs/script (PIMPL; core::script is a PRIVATE dep).
//
//  The loading door is the SAME one flowforge_aot_test proves: NativeBackend
//  validates LUX_SCRIPT_ABI_VERSION, runs bind_host against the resolver
//  (unresolved import = module rejected), and exposes the export table. This
//  file adds the entity-facing half: manifest pre-checks, per-asset module
//  sharing with instance refcounts, the per-instance state block that travels
//  through call_frame.user_context, and (ADR v2 §3.2) the event-entry
//  binding: every REGISTERED script event resolves against the loaded export
//  table by name, with call-frame slot templates built from the LOADED
//  signatures — the manifest is only a pre-check.
// ============================================================================

#include <lux/engine/ecs/script/backends/NativeModuleScriptBackend.hpp>

#include <lux/engine/function/script/ScriptHost.hpp>
#include <lux/engine/function/script/ScriptCallFrame.hpp>
#include <lux/engine/function/script/ScriptSignature.hpp>
#include <lux/engine/function/script/abi/lux_script_abi.h>
#include <lux/engine/function/script/backends/NativeBackend.hpp>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace lux::ecs
{
    namespace
    {
        std::uint64_t fnv1a64(std::span<const std::byte> bytes) noexcept
        {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const std::byte b : bytes)
            {
                h ^= static_cast<std::uint64_t>(b);
                h *= 0x100000001b3ull;
            }
            return h;
        }
    } // namespace

    struct NativeModuleScriptBackend::Impl
    {
        lux::script::ScriptHost host;

        struct ModuleEntry
        {
            lux::script::ModuleHandle handle         = lux::script::kInvalidModule;
            std::uint64_t             payload_hash   = 0;
            int                       live_instances = 0;
        };
        std::unordered_map<std::string, ModuleEntry> modules;   // key = cache_key (asset uuid)

        explicit Impl(HostSymbolResolver resolver)
        {
            host.registerBackend(lux::script::native_backend::create(
                std::move(resolver)));
        }

        void release(const std::string& key)
        {
            const auto it = modules.find(key);
            if (it == modules.end()) return;
            if (--it->second.live_instances > 0) return;
            host.unloadModule(it->second.handle);   // last instance gone → dll unloads
            modules.erase(it);
        }

        /// One bound event: the loaded module's function + the call-frame
        /// slot template derived from ITS signature (a module may take a
        /// SUBSET of the event payload — a parameterless OnUpdate is legal).
        struct EventEntry
        {
            const lux::script::ScriptFunction* fn = nullptr;
            std::vector<lux_script_value_slot> slot_template;
        };

        /// Per-entity instance state: shared module refcount + own state
        /// block + the dense [ScriptEventId] entry data the thunk consumes.
        struct InstanceState
        {
            Impl*                   owner = nullptr;
            std::string             key;
            World*                  world = nullptr;
            std::vector<std::byte>  block;
            std::vector<EventEntry> entries;
        };

        /// The one native dispatch thunk (every bound slot points here — the
        /// dispatcher's event id selects the entry).
        static bool eventThunk(void* state, ScriptEventId id, void* const* args)
        {
            auto* st = static_cast<InstanceState*>(state);
            const EventEntry& en = st->entries[id];

            lux_script_value_slot slots[ScriptEventRegistry::kMaxParams];
            const auto n = static_cast<std::uint32_t>(en.slot_template.size());
            for (std::uint32_t i = 0; i < n; ++i)
            {
                slots[i]      = en.slot_template[i];
                slots[i].data = args[i];
            }

            lux_script_call_frame raw{};
            raw.args          = n != 0 ? slots : nullptr;
            raw.arg_count     = n;
            raw.world_context = st->world;
            raw.user_context  = st->block.empty() ? nullptr : st->block.data();

            lux::script::CallFrame frame(&raw);
            // Hard faults propagate to the dispatcher's crash guard; a nonzero
            // ABI return is the script REPORTING failure → false disables it.
            return en.fn->invoke(frame);
        }

        static void dropInstance(void* state)
        {
            auto* st = static_cast<InstanceState*>(state);
            Impl*             owner = st->owner;
            const std::string key   = std::move(st->key);
            delete st;
            owner->release(key);
        }
    };

    NativeModuleScriptBackend::NativeModuleScriptBackend(HostSymbolResolver resolver)
        : impl_(std::make_unique<Impl>(std::move(resolver)))
    {
    }

    NativeModuleScriptBackend::~NativeModuleScriptBackend() = default;

    ScriptInstance
    NativeModuleScriptBackend::createInstanceFromAsset(
        lux::ecs::EntityHandle, World& world,
        const lux::rdesc::Script&  desc,
        std::span<const std::byte> payload,
        std::string_view           cache_key)
    {
        const auto* native = std::get_if<lux::rdesc::NativeModuleScript>(&desc.body);
        if (!native)
            return {};   // not mine — the registry tries the next backend

        // ── manifest pre-checks: reject before touching the loader ─────────
        if (native->abi_version != LUX_SCRIPT_ABI_VERSION)
        {
            std::fprintf(stderr,
                "[NativeModuleScriptBackend] '%s': manifest ABI v%u != host ABI "
                "v%u — module not loaded\n", desc.module_name.c_str(),
                native->abi_version, LUX_SCRIPT_ABI_VERSION);
            return {};
        }
        if (payload.empty())
        {
            std::fprintf(stderr,
                "[NativeModuleScriptBackend] '%s': empty payload (a native "
                "module asset carries the shared-library bytes)\n",
                desc.module_name.c_str());
            return {};
        }
        if (native->state_defaults.size() > native->state_size)
        {
            std::fprintf(stderr,
                "[NativeModuleScriptBackend] '%s': state_defaults (%zu B) "
                "exceed state_size (%u B) — corrupt recipe\n",
                desc.module_name.c_str(), native->state_defaults.size(),
                native->state_size);
            return {};
        }

        // ── module: one load per asset, shared by every instance ───────────
        const std::string   key(cache_key);
        const std::uint64_t hash = fnv1a64(payload);

        auto it = impl_->modules.find(key);
        if (it != impl_->modules.end() && it->second.payload_hash != hash)
        {
            // Changed payload under the same asset id. Play-boundary reload:
            // with live instances this cannot legally happen (asset payloads
            // are stable within a play session) — refuse rather than yank a
            // dll out from under running code.
            if (it->second.live_instances > 0)
            {
                std::fprintf(stderr,
                    "[NativeModuleScriptBackend] '%s': payload changed while "
                    "%d instance(s) are live — instance not created\n",
                    key.c_str(), it->second.live_instances);
                return {};
            }
            impl_->host.unloadModule(it->second.handle);
            impl_->modules.erase(it);
            it = impl_->modules.end();
        }
        if (it == impl_->modules.end())
        {
            // cache_key (the asset uuid) doubles as the host-side module name:
            // collision-free where module_name is only a display string.
            const auto handle =
                impl_->host.loadModuleFromMemory("native", payload, key);
            if (handle == lux::script::kInvalidModule)
            {
                std::fprintf(stderr,
                    "[NativeModuleScriptBackend] '%s': load rejected: %s\n",
                    desc.module_name.c_str(), impl_->host.lastError().c_str());
                return {};
            }
            // Manifest cross-check: every function the DESCRIPTION declares
            // must exist in the LOADED export table — drift between a cooked
            // manifest and its dll is a build bug, fail loudly.
            for (const auto& f : native->functions)
            {
                if (!impl_->host.findFunction(handle, f.name))
                {
                    std::fprintf(stderr,
                        "[NativeModuleScriptBackend] '%s': manifest declares "
                        "'%s' but the loaded module does not export it — "
                        "module rejected\n",
                        desc.module_name.c_str(), f.name.c_str());
                    impl_->host.unloadModule(handle);
                    return {};
                }
            }
            it = impl_->modules
                     .emplace(key, Impl::ModuleEntry{ handle, hash, 0 })
                     .first;
        }
        const auto handle = it->second.handle;

        // ── event binding (ADR v2 §3.2): registry × loaded export table ────
        const auto& evreg = scriptEventRegistry();
        auto state = std::make_unique<Impl::InstanceState>();
        state->owner = impl_.get();
        state->key   = key;
        state->world = &world;
        state->block.assign(native->state_size, std::byte{0});
        if (!native->state_defaults.empty())
            std::memcpy(state->block.data(), native->state_defaults.data(),
                        native->state_defaults.size());
        state->entries.resize(evreg.count());

        ScriptInstance inst(state.get(), &Impl::dropInstance);
        bool any_bound = false;
        for (ScriptEventId id = 0; id < evreg.count(); ++id)
        {
            const auto& ev = evreg.desc(id);
            const auto  fn = impl_->host.findFunction(handle, ev.name);
            if (!fn) continue;   // absent event — legal, slot stays empty
            const auto& sig = fn.fn->signature();
            if (sig.args.size() > ev.params.size())
            {
                std::fprintf(stderr,
                    "[NativeModuleScriptBackend] '%s': '%s' takes %zu args but "
                    "the event carries %zu — left unbound\n",
                    desc.module_name.c_str(), ev.name.c_str(),
                    sig.args.size(), ev.params.size());
                continue;
            }
            auto& entry = state->entries[id];
            entry.fn = fn.fn;
            entry.slot_template.resize(sig.args.size());
            for (std::size_t i = 0; i < sig.args.size(); ++i)
            {
                auto& slot   = entry.slot_template[i];
                slot         = {};
                slot.kind    = sig.args[i].kind;
                slot.size    = sig.args[i].size;
                slot.type_id = sig.args[i].type_id;
            }
            inst.bind(id, &Impl::eventThunk);
            any_bound = true;
        }
        if (!any_bound)
        {
            std::fprintf(stderr,
                "[NativeModuleScriptBackend] '%s': no export matches any "
                "registered script event — the instance is inert\n",
                desc.module_name.c_str());
        }

        ++it->second.live_instances;
        state.release();   // owned by inst's drop from here
        return inst;
    }
} // namespace lux::ecs
