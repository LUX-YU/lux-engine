#pragma once
// ============================================================================
//  DebugDraw.hpp — retained debug-line facade (Milestone D: script-driven
//  rendering).
//
//  The render backend (LineList) is a TRANSIENT feature: its buffer is taken
//  and cleared every frame, and the server-side TransientLineListBuffer is
//  last-writer-wins. Scripts however draw ONCE (a console Run, a flow-graph
//  Run) — so lines live here, in a retained CPU-side store, and the host
//  scene (EditorScene::updateSelectionGizmo) re-reads + re-uploads them every
//  frame, merged into its SINGLE per-frame uploadLines call.
//
//  THREADING CONTRACT: main thread ONLY. Writers are the editor's script
//  surfaces (LuaConsole Run, FlowGraphPanel Run) and C++ call sites on the
//  main thread; the reader is EditorScene::tick. A future game runtime that
//  scripts from another thread must revisit this (command queue), not relax
//  it silently.
//
//  The free functions below are the SCRIPT-FACING API:
//   - LUX_FUNC() marks them for the meta/lua generators (sol2 binding into
//     the `lux.*` table; same proven pattern as the script test's free fn).
//   - Scalar-only parameters BY DESIGN: the FlowForge MLIR lowering maps only
//     scalar types (records cannot lower), and the same flat signature keeps
//     the Lua call trivial. The flow JIT binds them by explicit name+address
//     (JitNativeSymbol) — no extern "C" needed anywhere.
// ============================================================================

#include <span>

#include <lux/engine/function/visibility.h>
#include <lux/engine/meta/MetaAnnotations.hpp>

namespace lux::gameplay::debugdraw
{
    /// One retained line segment in world space (color = linear RGB, 0..1).
    struct DebugLine
    {
        float from[3];
        float to[3];
        float color[3];
    };

    /// Host-side read API (NOT script-exposed): the owning scene reads this
    /// every frame and merges it into its single line-list upload.
    LUX_FUNCTION_PUBLIC std::span<const DebugLine> lines() noexcept;

    /// Drops all retained lines (host-side twin of lux_debug_draw_clear).
    LUX_FUNCTION_PUBLIC void clear() noexcept;

} // namespace lux::gameplay::debugdraw

// ---- script-facing API (global scope — generated lua bindings + flow JIT) ----

/// Adds a world-space line from (x0,y0,z0) to (x1,y1,z1) colored (r,g,b).
/// The line PERSISTS until lux_debug_draw_clear(). Main thread only.
LUX_FUNCTION_PUBLIC void LUX_FUNC() lux_debug_draw_line(
    float x0, float y0, float z0,
    float x1, float y1, float z1,
    float r, float g, float b);

/// Removes every line added by lux_debug_draw_line. Main thread only.
LUX_FUNCTION_PUBLIC void LUX_FUNC() lux_debug_draw_clear();
