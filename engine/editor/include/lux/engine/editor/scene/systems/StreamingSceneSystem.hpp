#pragma once
/**
 * @file StreamingSceneSystem.hpp
 * @brief Editor scene system: cell-based world streaming + its scene-global
 *        tuning + the cull-distance mirror to render + CPU-data eviction.
 *
 * Replaces the streaming blocks that used to be hardcoded inline in
 * EditorScene::tick (the OCP smell the user flagged). A scene that doesn't want
 * streaming simply doesn't register this system. It owns its WorldStreamingSystem
 * + dirty-tracking state (encapsulation that was leaking into EditorScene's
 * members), and brackets the renderable-update anchor:
 *   onPreRenderableUpdate  — resolve SceneSettings → params (dirty-applied) +
 *                            mirror cull_distance to render SpatialCull + tag
 *                            dormant cells (BEFORE the entt→GPU bridge reaps).
 *   onPostRenderableUpdate — evict CPU data of cells whose GPU instances the
 *                            bridge just reaped (AFTER it).
 */

#include <lux/engine/editor/scene/EditorSceneSystem.hpp>
#include <lux/engine/gameplay/3d/world/systems/WorldStreamingSystem.hpp>
#include <lux/engine/asset/Asset.hpp>   // asset_id_t (load sink)

#include <cstdint>
#include <functional>

namespace lux::editor
{
    class StreamingSceneSystem final : public EditorSceneSystem
    {
    public:
        /// @p load_sink forwards async asset loads for the prefetch path (was
        /// EditorScene::request_load_). Streaming is enabled-by-default but its
        /// ranges come from the scene's SceneSettingsComponent.
        explicit StreamingSceneSystem(
            std::function<void(const lux::asset::asset_id_t&)> load_sink);

        void onPreRenderableUpdate(const SceneTickContext& ctx) override;
        void onPostRenderableUpdate(const SceneTickContext& ctx) override;

    private:
        lux::gameplay::d3::WorldStreamingSystem streaming_;

        // Dirty-tracking: re-push streaming params + the cull mirror (a comm push)
        // ONLY when a SceneSettings value changes (encapsulated here, was 7 stray
        // scalars on EditorScene).
        bool          applied_{false};
        float         last_cull_distance_{0.f};
        bool          last_enabled_{false};
        float         last_cell_{0.f};
        float         last_load_{0.f};
        float         last_unload_{0.f};
        float         last_prefetch_{0.f};
        std::uint32_t last_evict_{0};
    };

} // namespace lux::editor
