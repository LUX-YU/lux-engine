#pragma once
/**
 * @file Scene3D.hpp
 * @brief Install hooks for the 3D scene kit.
 *
 * A `gameplay::World` is domain-neutral — it hardcodes no systems. These two
 * functions wire the standard 3D stack onto it:
 *   - `installSystems` adds the built-in World systems (Transform → Camera →
 *     Animation), replacing World's old hardcoded constructor;
 *   - `registerRenderables` registers the standard 3D renderable components on a
 *     RenderableSystem via their EcsRenderTraits.
 *
 * Forward-declared dependencies only — the heavy template instantiation (the
 * EcsRenderTraits specializations + bridge templates) is confined to Scene3D.cpp.
 */

#include <lux/engine/function/visibility.h>   // LUX_FUNCTION_PUBLIC
#include <lux/engine/render_bridge/ScenePackRegistry.hpp>
#include <lux/engine/ecs/World.hpp>
#include <lux/engine/render_bridge/RenderableSystem.hpp>

namespace lux::ecs { class World; }
namespace lux::render_bridge { class RenderableSystem; }

namespace lux::pack
{
    using lux::ecs::World;
    using lux::render_bridge::RenderableSystem;

    /// Add the standard 3D built-in systems to a neutral World, in canonical order
    /// (Transform → Camera → Animation). Replaces World's old hardcoded ctor. Call
    /// once right after constructing the World, BEFORE its first `tick()`.
    LUX_FUNCTION_PUBLIC void installSystems(lux::ecs::World& world);

    /// Register the standard 3D renderable components on a RenderableSystem via their
    /// EcsRenderTraits: 2 INSTANCE meshes (static + skeletal), Skybox + Grid (PARAM),
    /// and the 3 lights (POOL). Must run BEFORE the system's first `update()`.
    LUX_FUNCTION_PUBLIC void registerRenderables(lux::render_bridge::RenderableSystem& rs);

    /// Register the 3D pack on a ScenePackRegistry (ADR §3: one assembly API
    /// for every domain). The entry is UNCONDITIONAL — the 3D kit has no
    /// capability bits — so it backs nothing and always runs; a mixed 2D+3D
    /// scene assembles from one registry:
    ///     ScenePackRegistry reg; addD3Pack(reg); addD2Pack(reg);
    LUX_FUNCTION_PUBLIC void addD3Pack(lux::render_bridge::ScenePackRegistry& reg);

} // namespace lux::pack
