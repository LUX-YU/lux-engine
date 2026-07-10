#pragma once
/**
 * @file SceneSettingsComponent.hpp
 * @brief Scene-global view + streaming settings (the UE `WorldSettings` model).
 *
 * A single entity carrying this component is the scene's GLOBAL default for two
 * decoupled "view range" systems — mirroring how UE keeps them separate:
 *   - `cull_distance` (RENDER layer): loaded instances farther than this are not
 *     DRAWN (the render-side SpatialCull GPU cull). Like UE's draw distance /
 *     r.ViewDistanceScale. The editor mirrors it to the SpatialCull feature.
 *   - streaming ranges (GAMEPLAY layer): which cells/entities are LOADED vs
 *     unloaded around the camera (WorldStreamingSystem). Like UE World Partition.
 *
 * It is a SINGLETON component on purpose: it serializes with the scene for free
 * (the per-entity component path), survives scene reload, and is edited via the
 * dedicated "Scene Settings" panel — NOT hunted for in the Hierarchy. This is
 * exactly UE's WorldSettings (a singleton actor edited via the World Settings
 * panel). Per-entity overrides are a future, separate component.
 *
 * This is the single source of truth for the scene's streaming + cull config;
 * absent → the systems keep their built-in defaults. (Per-entity overrides are a
 * future, separate component.)
 *
 * Kept dependency-light (only <cstdint> + MetaAnnotations) so the editor-only
 * meta generator parses it cleanly. Defaults mirror the WorldStreamingSystem +
 * the SpatialCull defaults so a scene without this component is unchanged.
 */

#include <cstdint>

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/ecs/World.hpp>

namespace lux::pack
{
    using lux::ecs::World;

    struct LUX_COMPONENT() SceneSettingsComponent
    {
        // ── Render view distance (RENDER layer; drives SpatialCull cull) ──
        LUX_MEMBER(display_name=Cull Distance, min=16.0, tooltip=Loaded instances farther than this from the camera are not DRAWN (GPU cull). Independent of streaming; keep >= Unload Range to avoid drawing-gaps.)
        float cull_distance = 512.f;

        // ── World streaming (GAMEPLAY layer; load/unload of entities) ──
        LUX_MEMBER(display_name=Streaming Enabled, tooltip=Off -> everything stays loaded + active (no streaming))
        bool enabled = true;

        LUX_MEMBER(display_name=Stream Cell Size, min=1.0, tooltip=World units per streaming cell; smaller = finer / smoother)
        float cell_size = 64.f;

        LUX_MEMBER(display_name=Load Range, min=1.0, tooltip=Cells within this radius of the camera load + become visible)
        float load_range = 350.f;

        LUX_MEMBER(display_name=Unload Range, min=1.0, tooltip=Cells beyond this radius sleep/unload (>= Load Range; hysteresis band))
        float unload_range = 520.f;

        LUX_MEMBER(display_name=Prefetch Range, min=0.0, tooltip=Dormant cells within this radius pre-load their data to hide pop-in (> Unload Range))
        float prefetch_range = 720.f;

        LUX_MEMBER(display_name=Evict Age Frames, min=0, tooltip=Frames a cell must stay far before its CPU data is freed; 0 = never)
        std::uint32_t evict_age_frames = 90;
    };

} // namespace lux::pack
