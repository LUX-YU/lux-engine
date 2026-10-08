#include <lux/engine/function/script/lua/Lua.hpp>
#include <lux/engine/simulation/scripting/lua/LuaScriptBackend.hpp>

#include <lua.hpp>

#include <array>
#include <cassert>
#include <limits>
#include <type_traits>

namespace
{
    using namespace lux::script::lua;
    using namespace lux::simulation::script;
    unsigned prepared{}, finalized{};
    bool reject_preparation{};
    std::weak_ptr<int> error_owner;
    bool check_error_owner{};

    int releaseProbe(lua_State*) noexcept
    {
        if (check_error_owner)
            assert(!error_owner.expired());
        ++finalized;
        return 0;
    }

    bool prepareValue(lua_State* state) noexcept
    {
        ++prepared;
        lua_newuserdatauv(state, 1, 0);
        lua_newtable(state);
        lua_pushcfunction(state, &releaseProbe);
        lua_setfield(state, -2, "__gc");
        lua_setmetatable(state, -2);
        lua_setfield(state, LUA_REGISTRYINDEX, "lr01.prepared.probe");
        return !reject_preparation;
    }

    LuaScriptBackendConfig configuration() noexcept
    {
        LuaScriptBackendConfig config;
        config.instance_capacity = 2;
        config.prepared_call_capacity = 4;
        config.continuation_capacity = 2;
        config.execution_depth_capacity = 2;
        config.ability_catalog_method_capacity = 2;
        config.track_vm_allocations = true;
        return config;
    }
} // namespace

int main()
{
    static_assert(!std::is_constructible_v<ScriptEngine, LuaVmConfiguration>);
    static_assert(!std::is_move_constructible_v<ScriptEngine>);
    static_assert(!std::is_copy_constructible_v<ScriptEngine>);
    for (const auto bad_mode : {false, true})
    {
        LuaVmConfiguration config;
        if (bad_mode)
        {
            config.gc_mode = static_cast<ELuaGcMode>(255);
        }
        else
        {
            config.gc_parameters[0] = -2;
        }
        const auto invalid = ScriptEngine::create(config);
        assert(!invalid && invalid.error() == ELuaEngineError::INVALID_CONFIGURATION);
    }
    for (const auto mode : {ELuaGcMode::INCREMENTAL, ELuaGcMode::GENERATIONAL})
    {
        LuaVmConfiguration config;
        config.gc_mode = mode;
        config.track_allocations = true;
        auto engine = ScriptEngine::create(config);
        assert(engine && (*engine)->state());
        auto script = (*engine)->parseScript("lr01 = 21 * 2");
        assert(script && (*engine)->runScript(*script));
        lua_getglobal((*engine)->state(), "lr01");
        assert(lua_tointeger((*engine)->state(), -1) == 42);
        lua_pop((*engine)->state(), 1);
        assert((*engine)->allocationStats().enabled);
        assert(!(*engine)->parseScript("this is not Lua"));
    }
    {
        auto engine = ScriptEngine::create();
        assert(engine);
        auto owner = std::make_shared<int>(42);
        error_owner = owner;
        (*engine)->setOnError([owner = std::move(owner)](std::string_view) noexcept { assert(*owner == 42); });
        assert(prepareValue((*engine)->state()));
        check_error_owner = true;
        engine->reset(); // Lua finalizers precede callback-capture and allocator destruction.
        check_error_owner = false;
        assert(error_owner.expired() && prepared == finalized);
    }

    auto operation = makeLuaValueOperation<std::int32_t>();
    operation.prepare = &prepareValue;
    const std::array values{operation};
    const std::array valid_blocks{LuaPreparedBlockClass{2, 2}};
    const std::array oversized_blocks{LuaPreparedBlockClass{5, 1}};
    for (unsigned iteration{}; iteration < 32; ++iteration)
    {
        const auto original_prepared = prepared;
        auto config = configuration();
        config.values = values;
        config.prepared_ability_capacity = 4;
        config.prepared_ability_blocks = valid_blocks;
        config.prepared_ability_storage_bytes = 65536;
        config.prepared_event_capacity = 4;
        config.prepared_event_blocks = oversized_blocks;
        config.prepared_event_storage_bytes = 65536;
        auto invalid = LuaScriptBackend::create(config);
        assert(!invalid && invalid.error() == ELuaScriptBindingBackendError::INVALID_CAPACITY);
        assert(prepared == original_prepared && prepared == finalized);
        config.prepared_event_blocks = valid_blocks;
        config.prepared_event_storage_bytes = 1;
        invalid = LuaScriptBackend::create(config);
        assert(!invalid && invalid.error() == ELuaScriptBindingBackendError::INVALID_CAPACITY);
        config.prepared_event_storage_bytes = 65536;
        config.vm.gc_parameters[0] = -2;
        invalid = LuaScriptBackend::create(config);
        assert(!invalid && invalid.error() == ELuaScriptBindingBackendError::VM_CONFIGURATION_FAILURE);
        assert(prepared == original_prepared);
        config.vm = {};
        reject_preparation = true;
        invalid = LuaScriptBackend::create(config);
        assert(!invalid && invalid.error() == ELuaScriptBindingBackendError::VM_CONFIGURATION_FAILURE);
        assert(prepared == finalized); // Failed extension preparation closes the actual VM.
        reject_preparation = false;
        {
            auto backend = LuaScriptBackend::create(config);
            assert(backend && *backend);
            assert(prepared == finalized + 1);
            assert(backend->stats().prepared_binding_bytes > 0);
            auto replacement = LuaScriptBackend::create(config);
            assert(replacement && prepared == finalized + 2);
            *replacement = std::move(*backend);
            assert(!*backend && *replacement && prepared == finalized + 1);
        }
        assert(prepared == finalized);
        // No abilities or events is a complete supported configuration, not a failed pool.
        auto empty = LuaScriptBackend::create(configuration());
        assert(empty && *empty && empty->stats().prepared_ability_slots == 0);
    }
}
