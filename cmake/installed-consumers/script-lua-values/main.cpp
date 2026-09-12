#include "Values.lua.value.generated.hpp"
#include "Ability.ability.generated.hpp"
#include "Ability.ability.lua.generated.hpp"
#include <lux/engine/simulation/scripting/lua/LuaScriptBackend.hpp>
#include <lua.hpp>
#include <cassert>
#include <cstdio>
using namespace lux::script::lua;
void runInstalledRuntime();
int main()
{
    auto* state = luaL_newstate();
    assert(state && detail::LuaValueAccess::initialize(state));
    assert(luaL_loadstring(state, "return {key=7,weight=2.5}") == 0 && lua_pcall(state, 0, 1, 0) == 0);
    LuaValueReader input{state, 1};
    auto item = LuaValueCodec<Item>::read(input);
    assert(item && item->id == 7 && item->weight == 2.5);
    LuaValueWriter output{state};
    assert(LuaValueCodec<Item>::push(output, *item) && lua_gettop(state) == 2);
    const auto contribution = makeScriptAbilityLuaContribution<ValueAbility>();
    assert(contribution.valid() && contribution.methods.size() == 1);
    assert(contribution.methods[0].parameters[0].readable && contribution.methods[0].results[0].writable);
    lua_close(state);
    std::printf("INSTALLED_VALUE fields=2 id=7 weight=2.5 representation=%llu rule=%u PASS\n",
        LuaValueCodec<Item>::representation(), ConsumerRuleRevision);
    runInstalledRuntime();
}
