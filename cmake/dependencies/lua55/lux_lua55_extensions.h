#ifndef LUX_LUA55_EXTENSIONS_H
#define LUX_LUA55_EXTENSIONS_H

#ifdef __cplusplus
extern "C" {
#endif
#include <lua.h>

#if defined(LUX_LUA55_LEAF_YIELD_REVISION)
/* Only engine leaf primitives return this private sentinel; other shapes use standard Lua55 yield. */
LUA_API int luxlua_yieldleaf(lua_State* state, lua_KContext context, lua_KFunction continuation);
/* VM-wide, including live and released threads. Returns zero when not collected. */
LUA_API int luxlua_vmleafstats(lua_State* state, unsigned long long* fast, unsigned long long* fallback);
LUA_API int luxlua_vmcallinfostats(lua_State* state, unsigned long long* inlined, unsigned long long* heap);
#endif
#ifdef __cplusplus
}
#endif

#endif
