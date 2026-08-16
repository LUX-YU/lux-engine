#pragma once
/**
 * @file SceneFeatureSettingPanel.hpp
 * @brief Per-scene render-feature settings panel (feature-driven).
 *
 * Features are SCENE-SPECIFIC, so this panel AUTO-ENUMERATES the render features
 * the current scene uses (via the QueryFeatureParams comm op) and shows each
 * feature's config: the left pane lists every feature; clicking one shows its
 * editable parameters on the right, enumerated GENERICALLY via the reflection
 * registry + WidgetDispatch (the same machinery the InspectorPanel uses). A
 * feature that exposes no tunable params (RenderFeature::paramStructName() == "")
 * shows "No editable parameters." "Apply" commits the edits. A "Render Graph"
 * section dumps the scene's compiled graph for debugging.
 * See .internal/feature-quality-tiers-design-2026-06-19.md.
 *
 * INC-A scope: the read/enumerate + display path is fully generic; the only
 * feature that currently exposes params is Tonemap, whose Apply still rides its
 * existing per-feature op (tonemap_ops_). INC-B generalizes Apply (per-feature
 * op auto-bound from the query + per-field dirty tracking + batch send).
 */

#include <lux/engine/editor/visibility.h>
#include <lux/engine/ui/Panel.hpp>
#include <lux/engine/ui/WidgetDispatch.hpp>
#include <lux/engine/authoring/world/WorldTerrainAuthoring.hpp>
#include <lux/engine/editor/app/Selection.hpp>
#include <lux/engine/ecs/render/components/3d/DirectionalLightComponent.hpp>
#include <lux/engine/ecs/render/components/3d/HeightFogComponent.hpp>
#include <lux/engine/ecs/render/components/3d/SkyboxComponent.hpp>

#include <lux/engine/function/render/client/core/FeatureHandle.hpp>
#include <lux/engine/function/render/client/core/RenderSceneId.hpp>
#include <lux/engine/function/render/client/RenderProtocol.hpp>          // RenderGraphDumpReply / QueryFeatureParamsReply
#include <lux/engine/function/render/client/RenderRequest.hpp>    // RenderRequest<>
#include <lux/engine/function/render/client/FeatureCatalog.hpp>   // feature name → setParams op

#include <cstdint>
#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace lux::meta   { struct RefClass; }
namespace lux::render
{
    class RenderControlSession;
    class RenderFrameSession;
}

namespace lux::editor
{
    class EditorScene;

    class LUX_EDITOR_PUBLIC SceneFeatureSettingPanel : public lux::ui::Panel
    {
    public:
        explicit SceneFeatureSettingPanel(std::string title);
        ~SceneFeatureSettingPanel() override = default;

        /// Wire the live render target once the server is up. @p registry (borrowed,
        /// outlives the panel) provides each feature's GENERIC setParams op BY NAME
        /// for the Apply path — so ANY param-exposing feature (incl. plugins) is
        /// editable, not just Tonemap (INC-B). Borrowed session.
        void setTarget(lux::render::RenderFrameSession* frame,
                       lux::render::RenderControlSession* control,
                       lux::render::RenderSceneId scene_id,
                       const lux::render::FeatureCatalog* registry) noexcept
        {
            frame_           = frame;
            control_         = control;
            scene_id_        = scene_id;
            registry_        = registry;
            query_requested_ = true;   // enumerate the scene's features on next tick
        }

        /// Frame-OPEN step: issue the render commands the UI requested this frame
        /// (feature enumerate query / Apply / Dump) and poll their replies. MUST
        /// be called by the host between beginFrame() and trySubmitFrame() — paint()
        /// runs BEFORE beginFrame() (frame closed), so recording a command there
        /// would be wiped by the next begin(). Same contract the thumbnail/
        /// material-preview services follow. No-op until setTarget() wires a
        /// live session.
        void tickRender();

        /// Wire a getter for the current scene's SceneSettingsComponent (returned as
        /// `void*` to keep the panel free of gameplay types — the Scene tab passes it
        /// to drawReflectedStruct by its reflected name). Returns nullptr when no
        /// scene is live. Edits are live: EditorScene dirty-applies them (streaming +
        /// the cull mirror), so the Scene tab needs no Apply button.
        void setSceneSettingsAccessor(std::function<void*()> getter) noexcept
        {
            scene_settings_accessor_ = std::move(getter);
        }

        void setEditorScene(EditorScene* scene) noexcept
        {
            editor_scene_ = scene;
            last_terrain_selection_.reset();
            visual_state_loaded_ = false;
        }

    private:
        void paint() override;

        /// "Scene" tab: reflected Render View settings plus the generic
        /// Authoring contribution inventory.
        void drawSceneTab();

        /// "Features" tab: the existing per-feature settings (left list + right
        /// reflected param editor + Apply).
        void drawFeaturesTab();
        void drawTerrainTab();
        void drawSceneContributions();
        void drawVisualStateEditor();

        /// Right pane for the selected feature: generic reflected param table +
        /// Apply, or "No editable parameters." when the feature exposes none.
        void drawFeatureEditor();

        /// Generic reflected-struct editor: enumerate `struct_name`'s fields and
        /// draw an editable widget for each into `instance`. Returns false if the
        /// type isn't in the reflection registry (sidecar not loaded).
        bool drawReflectedStruct(const char* struct_name, void* instance);

        /// "Render Graph" debug section: a button asks the render thread to copy
        /// the compiled graph text into our buffer (dst_ptr idiom), shown here.
        void drawRenderGraphDump();

        /// Parse a QueryFeatureParams reply buffer into features_.
        void parseFeatureQuery(std::uint32_t count, std::size_t bytes);

        /// One enumerated feature + its local edit buffer.
        struct FeatureEntry
        {
            lux::render::FeatureHandle id{};   // full generational handle
            bool                     enabled{false};
            std::string              name;          ///< display name
            std::string              struct_name;   ///< reflected param type, "" = no params
            std::vector<std::uint8_t> edit_bytes;   ///< editable param snapshot (size = paramSize)
        };

        lux::ui::WidgetDispatch          dispatch_;

        lux::render::RenderFrameSession*   frame_{nullptr};
        lux::render::RenderControlSession* control_{nullptr};
        lux::render::RenderSceneId          scene_id_{};
        const lux::render::FeatureCatalog* registry_{nullptr};  ///< borrowed; feature name → setParams op
        std::function<void*()>              scene_settings_accessor_{};  ///< → current scene's SceneSettingsComponent (void*)
        EditorScene*                        editor_scene_{nullptr};
        std::array<char, 37u>               terrain_id_text_{};
        std::int64_t                        terrain_cell_a_{0};
        std::int64_t                        terrain_cell_b_{0};
        int                                 terrain_neighbourhood_{1};
        float                               terrain_local_x_{64.0f};
        float                               terrain_local_z_{64.0f};
        lux::authoring::WorldTerrainBrush   terrain_brush_{};
        std::array<char, 512u>              terrain_raw16_path_{};
        std::optional<TerrainSelection>
                                            last_terrain_selection_;
        lux::ecs::SkyboxComponent           skybox_edit_{};
        lux::ecs::DirectionalLightComponent directional_light_edit_{};
        lux::ecs::HeightFogComponent        height_fog_edit_{};
        bool                                visual_state_loaded_{false};
        std::array<char, 37u>               sky_texture_id_text_{};

        std::vector<FeatureEntry>        features_;             ///< enumerated from the scene
        int                              selected_feature_{0};  ///< index into features_

        // Intent flags set by paint() (frame CLOSED), consumed by tickRender()
        // (frame OPEN) — the commands cannot be issued from paint().
        bool                             apply_requested_{false}; ///< Apply clicked this frame
        bool                             dump_requested_{false};  ///< Dump clicked this frame

        // Feature enumeration: in-memory dst_ptr request → poll → parse.
        std::vector<char>                query_buf_{};
        lux::render::RenderRequest<lux::render::QueryFeatureParamsReply> query_req_{};
        bool                             query_inflight_{false};
        bool                             query_requested_{false}; ///< re-enumerate on next tick

        // Render-graph dump (debug): in-memory dst_ptr request → poll → show.
        std::string                      graph_dump_text_{};    ///< last read dump
        std::vector<char>                dump_buf_{};           ///< caller buffer the render thread fills
        lux::render::RenderRequest<lux::render::RenderGraphDumpReply> dump_req_{}; ///< in-flight request
        bool                             dump_inflight_{false};
    };

} // namespace lux::editor
