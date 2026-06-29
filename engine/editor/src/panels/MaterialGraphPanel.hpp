#pragma once
// ============================================================================
//  MaterialGraphPanel — the material graph editor, rebuilt as a THIN GraphKit
//  host (the second panel on the shared framework, after FlowGraphPanel).
//
//  The panel owns the domain SSOT (rdesc::MaterialGraph — the same object the
//  GLSL compiler lowers) plus the two domain adapters, and embeds ONE
//  lux::graphkit::GraphEditor that does everything canvas-shaped: blueprint
//  chrome, right-click fuzzy palette, drag-connect with schema validation,
//  undoable add/remove/connect/move, and position sync against the domain's
//  per-node ui_pos (persisted since LMGR v2 — reopen restores the layout).
//
//  Compile (lowerToIR -> emitGlsl -> compileToSpirv), the live preview, the
//  texture picker, the floating color picker and "Save as Asset" all stay
//  panel-side.
//
//  Private editor header (engine/editor/src — not installed).
// ============================================================================

#include <lux/engine/ui/Panel.hpp>
#include <lux/engine/ui/PreviewViewElement.hpp>
#include <lux/engine/ui/SearchPopupElement.hpp>
#include <lux/engine/description/material_graph/MaterialGraph.hpp>
#include <lux/engine/graphkit/GraphEditor.hpp>
#include <lux/engine/asset/Asset.hpp>   // asset_id_t

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace lux::render { struct GraphMaterialData; }
namespace lux::asset  { class  AssetManager; }

namespace lux::editor
{
    class PreviewScene;
    class AssetRegistry;
    class EditorTextureCache;
    struct EditorEvents;

    /// IGraphView adapter over a borrowed rdesc::MaterialGraph.
    ///  - GraphNodeRef.id  = rdesc node_id (monotonic, never recycled).
    ///  - GraphPinRef      = (node id, side, index into inputs()/outputs()).
    ///  - canvas position lives in Node::ui_pos/ui_placed (persisted, LMGR v2).
    ///  - detach/attach ride MaterialGraph::extractNode / addNodeWithId — the
    ///    id-stability contract for undo.
    ///  - connect REJECTS an occupied input (port contract (b)); the command
    ///    stack pre-disconnects cap-1 pins, making replace-on-reconnect one
    ///    undoable transaction (an upgrade over the old silent overwrite).
    class MaterialGraphView final : public lux::graphkit::IGraphView
    {
    public:
        explicit MaterialGraphView(lux::rdesc::MaterialGraph& graph)
            : graph_(&graph)
        {
        }

        void forEachNode(
            const std::function<void(lux::graphkit::GraphNodeRef, std::string_view)>& fn)
            const override;
        std::uint32_t pinCount(lux::graphkit::GraphNodeRef node,
                               lux::graphkit::EPinSide side) const override;
        lux::graphkit::GraphPinView pin(lux::graphkit::GraphNodeRef node,
                                        lux::graphkit::EPinSide side,
                                        std::uint32_t index) const override;
        void forEachLink(
            const std::function<void(lux::graphkit::GraphLinkView)>& fn) const override;
        std::optional<lux::graphkit::GraphVec2>
             nodePos(lux::graphkit::GraphNodeRef node) const override;
        void setNodePos(lux::graphkit::GraphNodeRef node,
                        lux::graphkit::GraphVec2 pos) override;

        lux::graphkit::GraphNodeRef addNode(std::string_view template_id) override;
        lux::graphkit::NodeCapture  detachNode(lux::graphkit::GraphNodeRef node) override;
        bool attachNode(lux::graphkit::GraphNodeRef original,
                        lux::graphkit::NodeCapture capture) override;
        bool connect(lux::graphkit::GraphPinRef from, lux::graphkit::GraphPinRef to) override;
        bool disconnect(lux::graphkit::GraphPinRef from,
                        lux::graphkit::GraphPinRef to) override;
        void reconstructNode(lux::graphkit::GraphNodeRef node) override;

    private:
        lux::rdesc::MaterialGraph* graph_;
        /// pin() label scratch — snapshot contract: the returned string_view is
        /// only valid during the enclosing call (the editor consumes it
        /// immediately).
        mutable std::string        label_scratch_;
    };

    /// IGraphSchema adapter: connect-time type rule (exact / truncate / splat —
    /// the lowering's implicit conversions), the fixed palette table, per-value-
    /// type pin colors + per-kind header tints, and the four in-node body
    /// editors (Constant / Param / SampleTexture / Swizzle). Popup-shaped UI
    /// (texture picker, color picker) goes through the deferred popup queue;
    /// the panel draws it in panel space.
    class MaterialGraphSchema final : public lux::graphkit::IGraphSchema
    {
    public:
        MaterialGraphSchema(lux::rdesc::MaterialGraph& graph, MaterialGraphView& view)
            : graph_(&graph), view_(&view)
        {
        }

        /// Body edits that change the BAKED shader (constant literals, type
        /// selectors, swizzle picks) — the panel recompiles.
        void setOnStructureDirty(std::function<void()> fn) { on_structure_dirty_ = std::move(fn); }
        /// Param value edits — live preview update, NO recompile.
        void setOnParamsDirty(std::function<void()> fn) { on_params_dirty_ = std::move(fn); }
        /// SampleTexture body: display name of the texture bound to a slot.
        void setBoundTextureName(std::function<std::string(std::uint32_t)> fn)
        {
            bound_texture_name_ = std::move(fn);
        }

        lux::graphkit::ConnectResult canConnect(lux::graphkit::GraphPinRef from,
                                                lux::graphkit::GraphPinRef to,
                                                const lux::graphkit::IGraphView& view)
            const override;
        std::span<const lux::graphkit::NodeTemplate> palette() const override;
        lux::graphkit::PinStyleDesc pinStyle(const lux::graphkit::GraphPinType& type)
            const override;
        lux::graphkit::NodeStyleDesc nodeStyle(lux::graphkit::GraphNodeRef node,
                                               const lux::graphkit::IGraphView& view)
            const override;
        void drawNodeBody(lux::graphkit::GraphNodeRef node, lux::graphkit::IGraphView& view,
                          lux::graphkit::DeferredPopupQueue& popups) override;

    private:
        lux::rdesc::MaterialGraph* graph_;
        MaterialGraphView*         view_;

        std::function<void()>                      on_structure_dirty_;
        std::function<void()>                      on_params_dirty_;
        std::function<std::string(std::uint32_t)>  bound_texture_name_;
    };

    class MaterialGraphPanel : public lux::ui::Panel
    {
    public:
        explicit MaterialGraphPanel(std::string title);
        ~MaterialGraphPanel() override;

        /// Bind a live preview scene (display + orbit). Wires the embedded
        /// PreviewViewElement to it and pushes the current graph for preview.
        void setPreviewScene(PreviewScene* preview);

        /// Wire the project asset index + the texture-upload cache (for the
        /// SampleTexture picker + live texture preview) + the asset manager (for
        /// "Save as Asset") + the editor event hub (to announce a saved material).
        /// registry/cache/events borrowed (owned by host); manager shared (used to
        /// create + register + export the baked asset).
        void setAssetServices(AssetRegistry* registry, EditorTextureCache* cache,
                              std::shared_ptr<lux::asset::AssetManager> manager,
                              EditorEvents* events);

        /// Open a material asset in the editor as a graph (UE-style: double-click
        /// any material -> material editor). The persisted graph carries node
        /// positions since LMGR v2; v1 assets open unplaced (grid layout) and
        /// upgrade on the next save.
        void openAsset(const lux::asset::asset_id_t& id);

    private:
        void paint() override;

        void buildDefaultGraph();
        void compile();
        void drawToolbar();
        /// Graph-level render-state authoring (alpha mode/cutoff/double-sided).
        void drawGraphProperties();

        /// Bake the current graph into a MaterialAsset (see saveAsAsset in the
        /// cpp for the shared bake recipe) + the modal name popup.
        bool saveAsAsset(const std::string& name, std::string* err);
        void drawSavePopup();

        lux::render::GraphMaterialData buildGraphMaterial();
        void                          openTexturePicker(std::uint32_t slot);
        std::string                   boundTextureName(std::uint32_t slot) const;

        /// Reset transient editor state + recompile after graph_ was replaced.
        void        resetEditorForNewGraph();
        std::string textureDisplayName(const lux::asset::asset_id_t& id) const;

        // Domain SSOT + the two stateless adapters + the shared editor.
        lux::rdesc::MaterialGraph           graph_;
        MaterialGraphView                   view_{ graph_ };
        MaterialGraphSchema                 schema_{ graph_, view_ };
        lux::graphkit::GraphEditor          editor_;
        std::uint64_t                       last_seen_revision_{ 0 }; ///< bake-on-edit poll

        std::string                         status_{ "Press Compile" };
        std::string                         glsl_;
        bool                                show_glsl_{ false };

        // Param whose color is being edited in the floating picker (invalid = none).
        lux::rdesc::node_id                 color_pick_node_{ lux::rdesc::invalid_node };

        // Live preview (display + orbit input) + the scene it drives (borrowed).
        lux::ui::PreviewViewElement         preview_element_{ "MaterialPreview" };
        PreviewScene*                       preview_{ nullptr };
        bool                                show_preview_{ false }; // opt-in via the toolbar button

        // SampleTexture slot -> bound texture (uuid + cached display name) + the
        // async-upload re-push tracker; borrowed asset services + the picker popup.
        struct TexBinding { lux::asset::asset_id_t uuid; std::string name; };
        std::unordered_map<std::uint32_t, TexBinding> slot_texture_;
        std::uint32_t                       last_tex_bindless_[8]{};
        AssetRegistry*                      registry_{ nullptr };
        EditorTextureCache*                 texture_cache_{ nullptr };
        EditorEvents*                       events_{ nullptr };  ///< borrowed; emit content_changed on save
        lux::ui::SearchPopupElement         texture_popup_{ "Pick Texture##matgraph" };

        // "Save as Asset": the manager (create + register + export) + the modal
        // name-input popup state.
        std::shared_ptr<lux::asset::AssetManager> asset_manager_;
        bool                                save_popup_open_{ false };
        std::string                         save_name_;
        std::string                         save_status_;   // popup error feedback
    };
}
