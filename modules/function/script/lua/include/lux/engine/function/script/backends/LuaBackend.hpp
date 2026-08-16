#pragma once
/**
 * @file LuaBackend.hpp
 * @brief Lua backend implementation for `lux::script::ScriptHost`.
 *
 * Loads `.lua` source files. Functions inside the chunk become callable through
 * @ref lux::script::ScriptHost::findFunction. The wrapper keeps a long-lived
 * `lua_State` per module and references each Lua function via the registry.
 *
 * Note: marshalling between `lux::script::CallFrame` and Lua values is
 * currently limited to the primitive @ref lux::script::ValueKind variants;
 * struct-ref and object-ptr passthrough is forwarded as light userdata only.
 */

#include <memory>

#include <lux/engine/function/visibility.h>
#include <lux/engine/function/script/ScriptBackend.hpp>

namespace lux::script::lua_backend
{
    /** Build a fresh Lua backend instance. */
    LUX_FUNCTION_PUBLIC std::unique_ptr<lux::script::IScriptBackend> create();
}
