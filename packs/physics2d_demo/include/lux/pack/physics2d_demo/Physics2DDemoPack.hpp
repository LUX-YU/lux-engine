#pragma once
// ============================================================================
//  Physics2DDemoPack.hpp — the demo physics pack's registry entry (ADR §3).
//
//  THE first external pack: it backs D2Capability::Physics/CharacterController
//  from OUTSIDE the 2D pack — pack_d2 contains zero solver types, zero solver
//  links. Assembly is the caller's choice:
//
//      render_bridge::ScenePackRegistry reg;
//      pack::addD2Pack(reg);
//      pack::addPhysics2DDemoPack(reg);     // ← this pack opts in
//      pack::install(world, services, reg, plan);
//
//  The entry runs AFTER the d2 entries (order 70): it drains the
//  CollisionProbes2D seam the domain side filled (the pixel field's adapter,
//  future tilemap colliders), owns the solver in SceneServices, and wires the
//  SimulatePhysics phase on the shared fixed-step coordinator. A real physics
//  integration (Box2D/Jolt wrapper) replaces this pack with the same shape.
// ============================================================================

#include <lux/engine/function/visibility.h>
#include <lux/engine/render_bridge/ScenePackRegistry.hpp>

namespace lux::pack
{
    LUX_FUNCTION_PUBLIC void addPhysics2DDemoPack(lux::render_bridge::ScenePackRegistry& reg);

} // namespace lux::pack
