#ifndef LUX_LUA_BOUNDARY_H
#define LUX_LUA_BOUNDARY_H

#include <stdint.h>
#include <lux/engine/function/visibility.h>

typedef struct lua_State lua_State;
#define LUX_LUA_BOUNDARY_RETURN 0u
#define LUX_LUA_BOUNDARY_SUSPEND 1u
#define LUX_LUA_BOUNDARY_ERROR 2u

/* A worker returns normally, with all C++ temporaries destroyed. Error text,
 * when present, is already owned by the Lua stack; otherwise C formats the code. */
typedef struct LuxLuaBoundaryOutcome {
    uint32_t kind;
    int result_count;
    int error_code;
} LuxLuaBoundaryOutcome;
typedef LuxLuaBoundaryOutcome (*LuxLuaTypedWorker)(lua_State*);

#ifdef __cplusplus
extern "C" {
#endif
/* Upvalues 1..3 belong to the caller's prepared provenance. Upvalue 4 is a
 * full userdata containing the immutable worker pointer, never a C++ stack borrow. */
LUX_FUNCTION_PUBLIC int luxLuaBoundaryEntry(lua_State* state);
#ifdef __cplusplus
}
#endif
#endif
