#include <lua.h>
#include <lauxlib.h>
#include <stdio.h>
#if LUA_VERSION_RELEASE_NUM != 50501
#error Expected the qualified Lua 5.5.1 headers
#endif
int main(void)
{
    lua_State* state;
    if (lua_version(NULL) != LUA_VERSION_NUM)
    {
        puts("VM_VERSION: header/runtime version mismatch before creating state");
        return 13;
    }
    state = luaL_newstate();
    if (!state) return 1;
    lua_close(state);
    puts("VM_VERSION: matching header/runtime");
    return 0;
}
