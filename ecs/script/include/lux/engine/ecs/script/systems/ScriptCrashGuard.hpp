#pragma once
// ============================================================================
//  ScriptCrashGuard.hpp — native-script crash isolation (scripting ADR §6b).
//
//  USER HARD REQUIREMENT: a buggy script may crash the GAME, but must NOT crash
//  the ENGINE/EDITOR. The engine currently has ZERO crash isolation, so this is
//  new infrastructure.
//
//  guardedScriptCall runs a native script callback under a hardware-fault guard
//  (Windows SEH: access violation / integer divide-by-zero / stack overflow /
//  illegal instruction; POSIX signal version is a TODO — Windows is the primary
//  dev platform). Returns true on a clean run and false after a hardware fault;
//  the caller then
//  disables the offending ScriptComponent (and, in the editor, stops play +
//  restores the pre-play snapshot).
//
//  Lua does NOT need this: LuaJIT is memory-safe and its errors are caught by
//  lua_pcall inside the Lua backend. The guard is for native (C++ / FlowForge)
//  behaviors only; on the Lua path it is a harmless direct call.
//
//  CAVEAT (documented, not hidden): after a memory-corrupting fault the process
//  state may be unsound. "Best effort" here means: catch, log, disable the
//  script, and return control so the host can stop play and recover to a known
//  edit-mode snapshot — NOT "pretend nothing happened and keep simulating".
// ============================================================================

#include <lux/engine/function/visibility.h>
#include <lux/engine/ecs/Registry.hpp>   // entity_id

#include <functional>
#include <string_view>

namespace lux::ecs
{
    /// `fn` must follow the engine-wide no-exception callback contract.
    /// true = clean; false = hardware fault caught.
    [[nodiscard]] LUX_FUNCTION_PUBLIC bool guardedScriptCall(const std::function<void()>& fn);

    /// Report a caught script fault (logs; the editor phase also stops play + restores).
    LUX_FUNCTION_PUBLIC void reportScriptFault(lux::ecs::Entity entity, std::string_view phase);
} // namespace lux::ecs
