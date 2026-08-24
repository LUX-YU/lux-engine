#pragma once
/**
 * @file SceneSettingsComponent.hpp
 * @brief Scene-global render-view settings.
 *
 * Spatial residency is deliberately absent. Interest radii belong to the
 * selected 2D/3D spatial contribution; this component only controls the
 * Render View.
 *
 * It is a SINGLETON component on purpose: it serializes with the scene for free
 * (the per-entity component path), survives scene reload, and is edited via the
 * dedicated "Scene Settings" panel — NOT hunted for in the Hierarchy. This is
 * exactly UE's WorldSettings (a singleton actor edited via the World Settings
 * panel). Per-entity overrides are a future, separate component.
 *
 * Kept dependency-light (only MetaAnnotations) so the editor-only meta
 * generator parses it cleanly.
 */

#include <lux/engine/meta/MetaAnnotations.hpp>

namespace lux::ecs
{

    struct LUX_COMPONENT() SceneSettingsComponent
    {
        // ── Render view distance (RENDER layer; drives SpatialCull cull) ──
        LUX_MEMBER(display_name=Cull Distance, min=16.0, tooltip=Loaded instances farther than this from the camera are not drawn. Spatial residency distances are configured by the interest component.)
        float cull_distance = 32768.f;

    };

} // namespace lux::ecs
