#pragma once
/**
 * @file lux_script_abi.h
 * @brief C ABI for compiled script modules loaded as concrete NativeModule objects.
 *
 * Any compiled script artifact (FlowForge-MLIR native dll, hand-written native
 * plugin, future AOT Lua, ...) exposes a single C entry point:
 *
 *     extern "C" const lux_script_module_desc* lux_script_get_module();
 *
 * The host loads the dynamic library, calls that entry, validates the ABI
 * version, and registers the function table. All cross-FFI calls go through
 * @ref lux_script_call_frame, so the rest of the engine never needs to know
 * what produced the binary.
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Increment whenever the layout below changes in a non-additive way. */
#define LUX_SCRIPT_ABI_VERSION 2u

/** Symbol name compiled modules must export. */
#define LUX_SCRIPT_MODULE_ENTRY "lux_script_get_module"

/** Coarse value kinds passed across the ABI boundary. */
typedef enum lux_script_value_kind {
    LUX_SCRIPT_VK_VOID        = 0,
    LUX_SCRIPT_VK_BOOL        = 1,
    LUX_SCRIPT_VK_INT32       = 2,
    LUX_SCRIPT_VK_UINT32      = 3,
    LUX_SCRIPT_VK_INT64       = 4,
    LUX_SCRIPT_VK_UINT64      = 5,
    LUX_SCRIPT_VK_FLOAT       = 6,
    LUX_SCRIPT_VK_DOUBLE      = 7,
    LUX_SCRIPT_VK_STRING_VIEW = 8,  /**< {const char* data; uint32_t size;}  */
    LUX_SCRIPT_VK_OBJECT_PTR  = 9,  /**< Opaque engine object pointer        */
    LUX_SCRIPT_VK_STRUCT_REF  = 10  /**< Pointer + size + type_id            */
} lux_script_value_kind;

/** Semantic parameter passing mode. Returns must use VALUE. */
typedef enum lux_script_pass_mode {
    LUX_SCRIPT_PASS_VALUE     = 0,
    LUX_SCRIPT_PASS_CONST_REF = 1
} lux_script_pass_mode;

/** Description of a parameter / return slot. */
typedef struct lux_script_type_desc {
    const char* name;        /**< Diagnostic-only, may be NULL. */
    uint64_t    type_id;     /**< Engine-wide stable id (e.g. lux::meta hash). */
    uint32_t    size;        /**< Storage size in bytes. */
    uint32_t    align;       /**< Storage alignment in bytes. */
    uint8_t     kind;        /**< @ref lux_script_value_kind. */
    uint8_t     pass;        /**< @ref lux_script_pass_mode. */
    uint8_t     reserved[6];
} lux_script_type_desc;

/** A single argument / return slot in a call frame. */
typedef struct lux_script_value_slot {
    uint8_t  kind;       /**< @ref lux_script_value_kind. */
    uint8_t  reserved[3];
    uint32_t size;       /**< Bytes of valid data at @ref data. */
    uint64_t type_id;    /**< Matches @ref lux_script_type_desc::type_id. */
    void*    data;       /**< Mutable for returns, read-only for args. */
} lux_script_value_slot;

/** Per-call frame passed to compiled script functions. */
typedef struct lux_script_call_frame {
    const lux_script_value_slot* args;
    uint32_t                     arg_count;
    uint32_t                     reserved0;

    lux_script_value_slot*       returns;
    uint32_t                     return_count;
    uint32_t                     reserved1;

    void*                        world_context; /**< Opaque engine ptr. */
    void*                        user_context;  /**< Opaque per-call ptr. */
} lux_script_call_frame;

/** Function pointer signature for compiled script entries. */
typedef int (*lux_script_invoke_fn)(lux_script_call_frame* frame);

/** Description of a single exported function. */
typedef struct lux_script_function_desc {
    const char* name;        /**< Fully-qualified name, e.g. "BP_Player.OnHit". */
    uint64_t    symbol_id;   /**< Required stable id; zero is invalid. */

    const lux_script_type_desc* args;
    uint32_t                    arg_count;

    const lux_script_type_desc* returns;
    uint32_t                    return_count;

    lux_script_invoke_fn        invoke;
} lux_script_function_desc;

/** Description of a compiled module returned by @ref LUX_SCRIPT_MODULE_ENTRY. */
typedef struct lux_script_module_desc {
    const char* module_name;
    uint32_t    abi_version;  /**< Must equal @ref LUX_SCRIPT_ABI_VERSION. */
    uint32_t    reserved;

    uint64_t    state_layout_hash;
    uint32_t    state_size;
    uint32_t    state_align;

    const lux_script_function_desc* functions;
    uint32_t                        function_count;
    uint32_t                        reserved1;
} lux_script_module_desc;

/*---------------------------------------------------------------------------
 * Host binding (optional, ADDITIVE — does not bump LUX_SCRIPT_ABI_VERSION).
 *
 * A module that needs to call back INTO the engine (FlowForge-compiled
 * scripts calling reflected engine functions, hand-written plugins using
 * engine services) additionally exports @ref LUX_SCRIPT_BIND_HOST_ENTRY.
 * The host calls it exactly once per load, after validating the module
 * entry and BEFORE the first invoke. The module fills its internal import
 * table by calling `resolve(host_ctx, name)` once per imported symbol and
 * keeping the returned addresses (which stay valid for the module's whole
 * lifetime — they are engine functions).
 *
 * Registration-style binding instead of a DLL import table on purpose:
 * engine callees may have no linker-visible symbol at all (reflection
 * trampolines are addresses in a registry), a missing import becomes a
 * diagnosable load error instead of a loader crash, and a future hot
 * reload can re-bind.
 *
 * Return value: 0 when every import resolved; the count of unresolved
 * imports otherwise (the host must discard the module). Modules without
 * imports simply do not export the symbol.
 *-------------------------------------------------------------------------*/

/** Optional symbol name for host-import binding. */
#define LUX_SCRIPT_BIND_HOST_ENTRY "lux_script_bind_host"

/** Host-side resolver: imported symbol name -> address (NULL if unknown). */
typedef void* (*lux_host_resolve_fn)(void* host_ctx, const char* symbol_name);

/** Signature of the module's optional @ref LUX_SCRIPT_BIND_HOST_ENTRY export. */
typedef int (*lux_script_bind_host_fn)(lux_host_resolve_fn resolve,
                                       void*               host_ctx,
                                       uint32_t            host_abi_version);

/** Optional helper macro for module entry exports. */
#if defined(_WIN32)
#  define LUX_SCRIPT_EXPORT __declspec(dllexport)
#else
#  define LUX_SCRIPT_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif
