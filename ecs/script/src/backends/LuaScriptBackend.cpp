// ============================================================================
//  LuaScriptBackend.cpp — sol2 + LuaJIT live here and NOWHERE else (PIMPL).
//
//  A Lua script `return { OnUpdate = function(self, dt) … end }` becomes a
//  per-entity LuaScriptInstance. `self` is a ScriptEntity usertype bound to the
//  live entity. Calls use sol2 protected results; Lua failures become a disabled
//  instance and never use C++ exception control flow.
//
//  PHASE 2b — GENERIC reflection component access (no domain coupling):
//    local t = self:get("lux::ecs::Transform2DComponent")
//    t.rotation = t.rotation + dt          -- read/write ANY reflected scalar field
//  `self:get(fqn)` resolves the FQN via the injected ComponentTypeCatalog,
//  the LIVE component's void* + RefClass, and returns a ComponentProxy whose
//  __index/__newindex read/write fields by name (EBaseType-dispatched). The
//  module therefore links only ecs::core + core::meta — NO ecs::components (the
//  phase-2a Transform2D coupling is gone). Perf: the FQN string is interned by
//  LuaJIT (pointer compare) and the C++ side is an O(1) cached-map lookup.
//
//  LIMITS (2b): scalar fields only (bool/int/float/double). Record fields
//  (Eigen::Vector2f position) return nil until a nested proxy lands (2b+).
//  Reflected writes do NOT set a component's `dirty` flag (it is LUX_NO_MEMBER,
//  unreflected) — a consuming system must value-diff, or a self:touch() helper
//  arrives later. Do not hold a proxy across structural changes (entt storage
//  may relocate) — get it, use it, drop it within a call.
// ============================================================================

#include <lux/engine/ecs/script/backends/LuaScriptBackend.hpp>

#include <lux/engine/ecs/script/systems/ScriptSystem.hpp>   // ScriptContext (full def)
#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/ecs/World.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/input/ActionMapper.hpp>
#include <lux/engine/input/InputActionRegistry.hpp>

#include <sol/sol.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace lux::ecs
{
    namespace
    {
        // ── reflection scalar get/set by field name ──────────────────────────
        const lux::meta::RefField* findField(const lux::meta::RefClass& rc, std::string_view name)
        {
            for (const auto& f : rc.fields)
                if (f.name == name)
                    return &f;
            return nullptr;
        }

        std::optional<double> getScalar(const void* base, const lux::meta::RefField& f)
        {
            const auto* p = static_cast<const std::byte*>(base) + f.offset;
            switch (static_cast<lux::meta::EBaseType>(f.type.qtype.base))
            {
                case lux::meta::EBaseType::Bool:   return *reinterpret_cast<const bool*>(p) ? 1.0 : 0.0;
                case lux::meta::EBaseType::Int8:   return *reinterpret_cast<const std::int8_t*>(p);
                case lux::meta::EBaseType::Uint8:  return *reinterpret_cast<const std::uint8_t*>(p);
                case lux::meta::EBaseType::Int16:  return *reinterpret_cast<const std::int16_t*>(p);
                case lux::meta::EBaseType::Uint16: return *reinterpret_cast<const std::uint16_t*>(p);
                case lux::meta::EBaseType::Int32:  return *reinterpret_cast<const std::int32_t*>(p);
                case lux::meta::EBaseType::Uint32: return *reinterpret_cast<const std::uint32_t*>(p);
                case lux::meta::EBaseType::Int64:  return static_cast<double>(*reinterpret_cast<const std::int64_t*>(p));
                case lux::meta::EBaseType::Uint64: return static_cast<double>(*reinterpret_cast<const std::uint64_t*>(p));
                case lux::meta::EBaseType::Float:  return *reinterpret_cast<const float*>(p);
                case lux::meta::EBaseType::Double: return *reinterpret_cast<const double*>(p);
                default: return std::nullopt;   // Void / Record / Unknown — not a scalar
            }
        }

        bool setScalar(void* base, const lux::meta::RefField& f, double v)
        {
            auto* p = static_cast<std::byte*>(base) + f.offset;
            switch (static_cast<lux::meta::EBaseType>(f.type.qtype.base))
            {
                case lux::meta::EBaseType::Bool:   *reinterpret_cast<bool*>(p)          = (v != 0.0);                       return true;
                case lux::meta::EBaseType::Int8:   *reinterpret_cast<std::int8_t*>(p)   = static_cast<std::int8_t>(v);      return true;
                case lux::meta::EBaseType::Uint8:  *reinterpret_cast<std::uint8_t*>(p)  = static_cast<std::uint8_t>(v);     return true;
                case lux::meta::EBaseType::Int16:  *reinterpret_cast<std::int16_t*>(p)  = static_cast<std::int16_t>(v);     return true;
                case lux::meta::EBaseType::Uint16: *reinterpret_cast<std::uint16_t*>(p) = static_cast<std::uint16_t>(v);    return true;
                case lux::meta::EBaseType::Int32:  *reinterpret_cast<std::int32_t*>(p)  = static_cast<std::int32_t>(v);     return true;
                case lux::meta::EBaseType::Uint32: *reinterpret_cast<std::uint32_t*>(p) = static_cast<std::uint32_t>(v);    return true;
                case lux::meta::EBaseType::Int64:  *reinterpret_cast<std::int64_t*>(p)  = static_cast<std::int64_t>(v);     return true;
                case lux::meta::EBaseType::Uint64: *reinterpret_cast<std::uint64_t*>(p) = static_cast<std::uint64_t>(v);    return true;
                case lux::meta::EBaseType::Float:  *reinterpret_cast<float*>(p)         = static_cast<float>(v);            return true;
                case lux::meta::EBaseType::Double: *reinterpret_cast<double*>(p)        = v;                                return true;
                default: return false;
            }
        }

        const ComponentSchemaDescriptor* componentEntry(
            const ComponentTypeCatalog& components,
            std::string_view fqn) noexcept
        {
            return components.findByCppName(fqn);
        }

        /// A read/write view onto a live component's fields (by reflection).
        struct ComponentProxy
        {
            void*                                base  = nullptr;
            const lux::meta::RefClass*           rc    = nullptr;
            // Write-contract plumbing: a field poke bypasses entt's
            // signals, so a successful write fires entry->notify (patch<T> →
            // on_update) — event-driven consumers observe reflection writes.
            const lux::ecs::ComponentSchemaDescriptor* entry = nullptr;
            lux::meta::EntityRegistryBase*       reg   = nullptr;
            entt::entity                         ent   = entt::null;

            sol::object index(const std::string& field, sol::this_state ts) const
            {
                if (!rc || !base)
                    return sol::lua_nil;
                const auto* f = findField(*rc, field);
                if (!f)
                    return sol::lua_nil;
                const auto v = getScalar(base, *f);
                if (!v)
                    return sol::lua_nil;   // non-scalar field (record) — 2b+
                return sol::make_object(ts, *v);
            }
            void newindex(const std::string& field, sol::stack_object value)
            {
                if (!rc || !base)
                    return;
                const auto* f = findField(*rc, field);
                if (f && value.is<double>() && setScalar(base, *f, value.as<double>()))
                    if (entry && entry->operations.notify && reg)
                        entry->operations.notify(*reg, ent);
            }
        };

        /// The `self` a Lua callback receives — the live entity.
        struct ScriptEntity
        {
            lux::meta::EntityRegistryBase* reg = nullptr;
            entt::entity    ent = entt::null;
            const ComponentTypeCatalog* components = nullptr;

            sol::object get(const std::string& fqn, sol::this_state ts) const
            {
                const auto* e = components
                    ? componentEntry(*components, fqn)
                    : nullptr;
                if (!e || !e->ref_class || !e->operations.has ||
                    !e->operations.get)
                    return sol::lua_nil;
                if (!e->has(*reg, ent))
                    return sol::lua_nil;
                void* base = e->operations.get(*reg, ent);
                if (!base)
                    return sol::lua_nil;
                return sol::make_object(ts, ComponentProxy{base, e->ref_class, e, reg, ent});
            }
            [[nodiscard]] unsigned id() const { return static_cast<unsigned>(ent); }
        };

        struct CompiledScript
        {
            sol::table  callbacks;         ///< the chunk's returned table (event name → fn)
            std::size_t source_hash = 0;   ///< asset path: recompile gate (hot reload)
        };

        // ── Event-table dispatch (ADR v2 §3.2): per-instance state + ONE generic thunk ──
        // The instance state owns per-event sol handles, resolved ONCE at bind
        // from the module table by REGISTERED event name. The thunk marshals
        // the packed args to Lua values by the event's declared param types
        // (scalars only — a non-scalar param rejects the binding, loudly).

        /// Convert one packed arg to a Lua value by its declared RefType.
        /// Returns false for non-scalar types (bind-time rejects those, so
        /// this failing at call time means registry/binding drift).
        bool packedToLua(sol::state_view lua, const lux::meta::RefType& type,
                         const void* arg, sol::object& out)
        {
            switch (static_cast<lux::meta::EBaseType>(type.qtype.base))
            {
                case lux::meta::EBaseType::Bool:
                    out = sol::make_object(lua, *static_cast<const bool*>(arg));          return true;
                case lux::meta::EBaseType::Int8:
                    out = sol::make_object(lua, *static_cast<const std::int8_t*>(arg));   return true;
                case lux::meta::EBaseType::Uint8:
                    out = sol::make_object(lua, *static_cast<const std::uint8_t*>(arg));  return true;
                case lux::meta::EBaseType::Int16:
                    out = sol::make_object(lua, *static_cast<const std::int16_t*>(arg));  return true;
                case lux::meta::EBaseType::Uint16:
                    out = sol::make_object(lua, *static_cast<const std::uint16_t*>(arg)); return true;
                case lux::meta::EBaseType::Int32:
                    out = sol::make_object(lua, *static_cast<const std::int32_t*>(arg));  return true;
                case lux::meta::EBaseType::Uint32:
                    out = sol::make_object(lua, *static_cast<const std::uint32_t*>(arg)); return true;
                case lux::meta::EBaseType::Int64:
                    out = sol::make_object(lua, *static_cast<const std::int64_t*>(arg));  return true;
                case lux::meta::EBaseType::Uint64:
                    out = sol::make_object(lua, *static_cast<const std::uint64_t*>(arg)); return true;
                case lux::meta::EBaseType::Float:
                    out = sol::make_object(lua, *static_cast<const float*>(arg));         return true;
                case lux::meta::EBaseType::Double:
                    out = sol::make_object(lua, *static_cast<const double*>(arg));        return true;
                default:
                    return false;
            }
        }

        /// Whether every param of @p desc is Lua-marshallable (bind-time gate).
        bool luaMarshallable(const ScriptEventDesc& desc)
        {
            for (const auto& p : desc.params)
            {
                if (!p.type) return false;
                switch (static_cast<lux::meta::EBaseType>(p.type->qtype.base))
                {
                    case lux::meta::EBaseType::Void:
                    case lux::meta::EBaseType::Record:
                        return false;
                    default: break;
                }
            }
            return true;
        }

        struct LuaInstanceState
        {
            ScriptEntity                          self;
            sol::state_view                       lua;    // the backend's state
            std::vector<sol::protected_function>  fns;    // dense by ScriptEventId

            LuaInstanceState(ScriptEntity s, lua_State* L) : self(s), lua(L) {}
        };

        /// The one Lua dispatch thunk (every bound slot points here — the
        /// dispatcher's event id selects the handle).
        bool luaEventThunk(void* state, ScriptEventId id, void* const* args)
        {
            auto* st = static_cast<LuaInstanceState*>(state);
            const sol::protected_function& fn = st->fns[id];
            if (!fn.valid()) return true;   // unreachable when bound correctly

            const auto& desc = scriptEventRegistry().desc(id);
            const std::size_t n = desc.params.size();
            sol::object argv[ScriptEventRegistry::kMaxParams];
            for (std::size_t i = 0; i < n; ++i)
            {
                if (!packedToLua(st->lua, *desc.params[i].type, args[i], argv[i]))
                    return false;   // registry/binding drift — surface as fault
            }

            sol::protected_function_result r;
            switch (n)   // sol call sites need compile-time arity (≤ kMaxParams)
            {
                case 0: r = fn(st->self); break;
                case 1: r = fn(st->self, argv[0]); break;
                case 2: r = fn(st->self, argv[0], argv[1]); break;
                case 3: r = fn(st->self, argv[0], argv[1], argv[2]); break;
                case 4: r = fn(st->self, argv[0], argv[1], argv[2], argv[3]); break;
                case 5: r = fn(st->self, argv[0], argv[1], argv[2], argv[3],
                               argv[4]); break;
                case 6: r = fn(st->self, argv[0], argv[1], argv[2], argv[3],
                               argv[4], argv[5]); break;
                case 7: r = fn(st->self, argv[0], argv[1], argv[2], argv[3],
                               argv[4], argv[5], argv[6]); break;
                default: r = fn(st->self, argv[0], argv[1], argv[2], argv[3],
                                argv[4], argv[5], argv[6], argv[7]); break;
            }
            if (!r.valid())
            {
                const sol::error err = r;
                std::fprintf(stderr, "[lua %s] %s\n",
                             desc.name.c_str(), err.what());
                return false;   // script-reported failure → dispatcher disables
            }
            return true;
        }
    } // namespace

    struct LuaScriptBackend::Impl
    {
        sol::state                                      lua;
        std::unordered_map<std::string, CompiledScript> scripts;
        const ComponentTypeCatalog&                     components;

        // Current frame's input (set by beginFrame; read by the Lua `Input` table).
        const lux::input::ActionMapper*        input   = nullptr;
        const lux::input::InputActionRegistry* actions = nullptr;

        explicit Impl(const ComponentTypeCatalog& component_catalog)
            : components(component_catalog)
        {
            lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string,
                               sol::lib::table, sol::lib::os);
            lua.new_usertype<ComponentProxy>(
                "ComponentProxy",
                sol::meta_function::index,     &ComponentProxy::index,
                sol::meta_function::new_index, &ComponentProxy::newindex);
            lua.new_usertype<ScriptEntity>(
                "ScriptEntity",
                "get", &ScriptEntity::get,
                "id",  &ScriptEntity::id);

            // Global `Input` table — resolves action NAME → ActionId (registry) →
            // queries the current-frame mapper. Names are the authoring key.
            sol::table in = lua.create_named_table("Input");
            in.set_function("axis", [this](const std::string& name) -> float {
                if (!input || !actions) return 0.0f;
                const lux::input::ActionId id = actions->findByName(name);
                return id == lux::input::InvalidActionId ? 0.0f : input->getValue(id).as1D();
            });
            in.set_function("isDown", [this](const std::string& name) -> bool {
                if (!input || !actions) return false;
                const lux::input::ActionId id = actions->findByName(name);
                return id != lux::input::InvalidActionId && input->active(id);
            });
            in.set_function("triggered", [this](const std::string& name) -> bool {
                if (!input || !actions) return false;
                const lux::input::ActionId id = actions->findByName(name);
                return id != lux::input::InvalidActionId && input->triggered(id);
            });
        }
    };

    LuaScriptBackend::LuaScriptBackend(
        const ComponentTypeCatalog& components)
        : impl_(std::make_unique<Impl>(components))
    {}
    LuaScriptBackend::~LuaScriptBackend() = default;

    void LuaScriptBackend::beginFrame(const ScriptContext& ctx)
    {
        impl_->input   = ctx.input;
        impl_->actions = ctx.actions;
    }

    void LuaScriptBackend::registerLuaSource(std::string_view name, std::string source)
    {
        sol::load_result chunk = impl_->lua.load(source);
        if (!chunk.valid())
        {
            sol::error e = chunk;
            std::fprintf(stderr, "[lua] load '%.*s' failed: %s\n",
                         static_cast<int>(name.size()), name.data(), e.what());
            return;
        }
        sol::protected_function_result ran = chunk();
        if (!ran.valid())
        {
            sol::error e = ran;
            std::fprintf(stderr, "[lua] run '%.*s' failed: %s\n",
                         static_cast<int>(name.size()), name.data(), e.what());
            return;
        }
        sol::object ret = ran;
        if (!ret.is<sol::table>())
        {
            std::fprintf(stderr, "[lua] '%.*s' must `return` a table of callbacks\n",
                         static_cast<int>(name.size()), name.data());
            return;
        }
        sol::table t = ret;
        CompiledScript cs;
        cs.callbacks = t;   // the whole module table — events resolve by
                            // REGISTERED name at instance bind (ADR v2 §3.2)
        impl_->scripts[std::string(name)] = std::move(cs);
    }

    ScriptInstance
    LuaScriptBackend::createInstanceFromAsset(
        lux::meta::EntityHandle entity,
        World& world,
                                              const lux::rdesc::Script&  desc,
                                              std::span<const std::byte> payload,
                                              std::string_view           cache_key)
    {
        if (!std::holds_alternative<lux::rdesc::LuaSourceScript>(desc.body))
            return {};   // not our kind — let the next backend look

        // (Re)compile when unseen OR the source changed. Keying the compile
        // cache on the source hash makes HOT RELOAD a plain re-import: the
        // next play-start sees a different hash and recompiles — no dedicated
        // reload machinery. (LuaSourceScript::entry — multi-behavior selection
        // inside one chunk — is deferred; the chunk's returned table IS the
        // behavior for now.)
        const std::string      key{cache_key};
        const std::string_view source(reinterpret_cast<const char*>(payload.data()),
                                      payload.size());
        const std::size_t hash = std::hash<std::string_view>{}(source);

        auto it = impl_->scripts.find(key);
        if (it == impl_->scripts.end() || it->second.source_hash != hash)
        {
            registerLuaSource(key, std::string(source));
            it = impl_->scripts.find(key);
            if (it == impl_->scripts.end())
                return {};   // compile/run failed (already logged)
            it->second.source_hash = hash;
        }

        // ── bind (ADR v2 §3.2): resolve each REGISTERED event's handle from
        // the module table ONCE; only implemented events get an entry — the
        // subscription index then never dispatches this instance for others.
        const auto& evreg = scriptEventRegistry();
        auto state = std::make_unique<LuaInstanceState>(
            ScriptEntity{
                &world.registry(),
                entity.entity(),
                &impl_->components},
            impl_->lua.lua_state());
        state->fns.resize(evreg.count());

        ScriptInstance inst(
            state.get(),
            [](void* s) { delete static_cast<LuaInstanceState*>(s); });
        for (ScriptEventId id = 0; id < evreg.count(); ++id)
        {
            const auto& ev = evreg.desc(id);
            sol::object cb = it->second.callbacks[ev.name];
            if (!cb.is<sol::protected_function>())
                continue;   // absent event — legal, slot stays empty
            if (!luaMarshallable(ev))
            {
                std::fprintf(stderr,
                    "[lua] '%s' implements '%s' but a param type is not "
                    "Lua-marshallable — left unbound\n",
                    key.c_str(), ev.name.c_str());
                continue;
            }
            state->fns[id] = cb.as<sol::protected_function>();
            inst.bind(id, &luaEventThunk);
        }
        state.release();   // owned by inst's drop from here
        return inst;
    }
} // namespace lux::ecs
