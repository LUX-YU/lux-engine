#include <lux/engine/scene/scripting/AssetAbilityLua.hpp>
#include <lux/cxx/container/ScopeId.hpp>
#include <lua.hpp>
#include <cassert>
#include <cstdio>
#include <cstring>

using namespace lux;
using namespace lux::scene::script;
using namespace lux::script::lua;

template <class T> void roundTrip(lua_State* state, const T& value)
{
    assert(TLuaValueCodec<T>::prepare(state));
    const auto base = lua_gettop(state);
    LuaValueWriter output{state};
    assert(TLuaValueCodec<T>::push(output, value));
    assert(lua_type(state, -1) == LUA_TUSERDATA);
    LuaValueReader input{state, -1};
    const auto result = TLuaValueCodec<T>::read(input);
    assert(result);
    // Semantic fields are compared by callers. This checks the exact owned representation including full-width IDs.
    assert(std::memcmp(&*result, &value, sizeof(T)) == 0);
    lua_settop(state, base);
}

int main()
{
    lua_State* state = luaL_newstate();
    assert(state);
    luaL_openlibs(state);
    const asset::AssetId id{*uuids::uuid::from_string("fedcba98-7654-4abc-ffff-ffffffffffff")};
    roundTrip(state, id);
    cxx::ScopeIdSource<ScriptAssetScopeTag> domains;
    const ScriptAssetHandle handle{domains.acquire(), {0xfedcba98U, 0xfffffffeU}};
    roundTrip(state, handle);
    roundTrip(state, ScriptAssetReadOutcome::success(handle));
    roundTrip(state, ScriptAssetReadOutcome::failure(EScriptAssetFailureDomain::STORAGE, 0xfffffffeU));
    const ScriptAssetInspection inspected{{id, {}, 0xffffffffffffffffULL, false, true}, 0};
    roundTrip(state, inspected);
    ScriptAssetBytes chunk;
    chunk.value.size = 256;
    for (std::size_t i{}; i < chunk.value.size; ++i) chunk.value.bytes[i] = std::byte(i);
    roundTrip(state, chunk);

    LuaValueWriter output{state};
    assert(TLuaValueCodec<asset::AssetId>::push(output, id));
    LuaValueReader wrong_type{state, -1};
    assert(!TLuaValueCodec<ScriptAssetHandle>::read(wrong_type));
    lua_setglobal(state, "id");
    assert(luaL_dostring(state, "local ok=pcall(function() id.field=7 end); assert(not ok)") == LUA_OK);
    lua_createtable(state, 0, 2);
    lua_pushinteger(state, 123); lua_setfield(state, -2, "domain");
    LuaValueReader table{state, -1};
    assert(!TLuaValueCodec<ScriptAssetHandle>::read(table));
    lua_settop(state, 0);
    assert(output.opaque(handle, semantic::typeId(semantic::TTypeTraits<ScriptAssetHandle>::CanonicalName),
        TLuaValueCodec<ScriptAssetHandle>::representation() + 1));
    LuaValueReader wrong_version{state, -1};
    assert(!TLuaValueCodec<ScriptAssetHandle>::read(wrong_version));
    lua_settop(state, 0);
    lua_newuserdatauv(state, sizeof(handle) + 64, 0);
    LuaValueReader foreign{state, -1};
    assert(!TLuaValueCodec<ScriptAssetHandle>::read(foreign));
    lua_settop(state, 0);

    const auto contribution = assetAbilityLua();
    assert(contribution.valid() && contribution.methods.size() == 16);
    for (const auto& method : contribution.methods)
    {
        for (const auto& value : method.parameters) assert(value.readable && value.prepare(state));
        for (const auto& value : method.results) assert(value.writable && value.prepare(state));
    }
    lua_close(state);
    std::puts("EC2 opaque values: full identity, read-only, foreign/type/version rejection, generated contribution PASS");
}
