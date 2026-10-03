from pathlib import Path
import json
s=Path('E:/SyncForder/CodeRepos/lux-engine-ec2')
p=s/'engine/scene/scripting/assets/include/lux/engine/scene/scripting/AssetAbility.hpp';t=p.read_text().replace('std::int32_t releaseAsset','void releaseAsset');p.write_text(t)
p=s/'engine/scene/scripting/assets/src/AssetAbility.cpp';t=p.read_text().replace('[](void* context, ScriptAssetHandle handle) noexcept -> std::int32_t {\n                auto result = scope(context).releaseAsset(handle);\n                return result ? 0 : static_cast<std::int32_t>(result.error());\n            }','''[](void* context, ScriptAssetHandle handle) noexcept {
                // Script release is idempotent; it cannot release another instance's domain.
                // describeAsset reports precise validity; native clients retain the fallible release API.
                (void)scope(context).releaseAsset(handle);
            }''');p.write_text(t)
old=s/'engine/scene/scripting/assets/include/lux/engine/scene/scripting/AssetAbilityLua.hpp'
p=s/'engine/scene/scripting/assets_lua/include/lux/engine/scene/scripting/AssetAbilityLua.hpp';p.parent.mkdir(parents=True,exist_ok=True)
t=old.read_text().replace('#include <lux/engine/function/script/lua/LuaValue.hpp>','#include <lux/engine/function/script/lua/ScriptAbilityLua.hpp>');t=t.replace('#include <lux/engine/scene/scripting/AssetAbility.ability.lua.generated.hpp>','''namespace lux::scene::script
{
    [[nodiscard]] lux::script::lua::ScriptAbilityLuaContribution assetAbilityLua() noexcept;
}''');p.write_text(t);old.unlink()
p=s/'engine/scene/scripting/assets_lua/src/AssetAbilityLua.cpp';p.parent.mkdir(parents=True,exist_ok=True);p.write_text('''#include <lux/engine/scene/scripting/AssetAbilityLua.hpp>
#include <AssetAbility.ability.generated.hpp>
#include <AssetAbility.ability.lua.generated.hpp>

namespace lux::scene::script
{
    lux::script::lua::ScriptAbilityLuaContribution assetAbilityLua() noexcept
    {
        return lux::script::lua::makeScriptAbilityLuaContribution<AssetAbility>();
    }
}
''')
p=s/'engine/scene/scripting/assets_lua/CMakeLists.txt';p.write_text('''add_component(
    COMPONENT_NAME scene_script_assets_lua
    NAMESPACE lux::engine::scene
    OUTPUT_NAME lux_engine_scene_script_assets_lua
    STATIC
    SOURCE_FILES src/AssetAbilityLua.cpp
)
component_include_directories(scene_script_assets_lua
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME include
)
target_link_libraries(scene_script_assets_lua PUBLIC scene_script_assets simulation_script_lua)
get_target_property(_asset_generated scene_script_assets LUX_SCRIPT_ABILITY_GENERATED_DIR)
target_include_directories(scene_script_assets_lua PRIVATE ${_asset_generated})
add_dependencies(scene_script_assets_lua scene_script_assets_script_abilities_generate)
component_add_transitive_commands(scene_script_assets_lua
    "find_package(lux-engine-scene-script-assets REQUIRED COMPONENTS scene_script_assets)"
    "find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_script_lua)"
)
lux_engine_install_components(
    PROJECT_NAME lux-engine-scene-script-assets-lua VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::scene COMPONENTS scene_script_assets_lua
)
if(BUILD_TESTING)
    add_executable(scene_script_asset_values_test test/values.cpp)
    target_link_libraries(scene_script_asset_values_test PRIVATE scene_script_assets_lua LuxLua55::Runtime)
    if(MSVC)
        target_compile_options(scene_script_asset_values_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(scene_script_asset_values_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME scene.script_asset_values COMMAND scene_script_asset_values_test)
    set_tests_properties(scene.script_asset_values PROPERTIES TIMEOUT 30 LABELS "native;EC2")
endif()
''')
p=s/'engine/scene/CMakeLists.txt';t=p.read_text().replace('add_subdirectory(scripting/assets)','add_subdirectory(scripting/assets)\nadd_subdirectory(scripting/assets_lua)');p.write_text(t)
p=s/'editor/tests/architecture/rules.json';data=json.loads(p.read_text());
def find(d):
 if isinstance(d,dict):
  if 'scene_script_assets' in d:return d
  for x in d.values():
   f=find(x)
   if f is not None:return f
 return None
r=find(data)
for n,layer,role,path in [('scene_script_assets_script_abilities','ENGINE','GENERATOR','assets'),('scene_script_assets_script_abilities_generate','ENGINE','GENERATOR','assets'),('scene_script_assets_lua','ENGINE','PROVIDER','assets_lua'),('scene_script_asset_values_test','TEST','TEST','assets_lua')]:
 r[n]={'layer':layer,'role':role,'capabilities':['CPU','PROCESS'],'path':'engine/scene/scripting/'+path}
p.write_text(json.dumps(data,ensure_ascii=False,indent=2)+'\n')
p=s/'engine/scene/scripting/assets_lua/test/values.cpp';p.parent.mkdir(parents=True,exist_ok=True);p.write_text('''#include <lux/engine/scene/scripting/AssetAbilityLua.hpp>
#include <lux/cxx/container/ScopeId.hpp>
#include <lua.hpp>
#include <cassert>
#include <cstdio>

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
    const ScriptAssetHandle handle{domains.issue(), {0xfedcba98U, 0xfffffffeU}};
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
'''.replace('#include <cstdio>','#include <cstdio>\n#include <cstring>'))
