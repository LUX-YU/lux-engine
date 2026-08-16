#pragma once
// ============================================================================
//  FlowGraphPanel — the FlowForge graph editor, rebuilt as a THIN GraphKit
//  host (the first panel on the shared framework; replaces the retired
//  NodeEditorPanel and its hand-rolled canvas machinery).
//
//  The panel owns the domain SSOT (flowforge::FlowGraph — the same object the
//  MLIR compiler walks) plus the two domain adapters, and embeds ONE
//  lux::graphkit::GraphEditor that does everything canvas-shaped. Per the
//  GraphKit contract, the adapters are STATELESS lenses: graph data exists
//  exactly once, in the FlowGraph.
//
//  Private editor header (engine/editor/src, MaterialGraphPanel precedent —
//  not installed).
// ============================================================================

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <lux/engine/resource/asset/Asset.hpp>   // asset_id_t, EAssetType
#include <lux/engine/events/DomainEvents.hpp>
#include <lux/engine/authoring/flowforge/FlowGraph.hpp>
#include <lux/engine/authoring/flowforge/NodeRegistry.hpp>
#include <lux/engine/editor/framework/graphkit/GraphEditor.hpp>
#include <lux/engine/ui/Panel.hpp>

#if LUX_FLOWFORGE_HAS_MLIR
namespace lux::flowforge { class IRContext; class MLIRBuilder; }
#endif

namespace lux::asset { class AssetManager; }

namespace lux::editor
{
    class AssetRegistry;
    /// IGraphView adapter over a borrowed flowforge::FlowGraph.
    ///  - GraphNodeRef.id  = the FlowGraph sparse-set INDEX (NodeStorage.index;
    ///    recycled on delete — the GraphKit bimap purges eagerly).
    ///  - GraphPinRef      = (index, side, position in inPins()/outPins()).
    ///    Raw Pin* never crosses up (pins are deleted on rebind/reconstruct).
    ///  - canvas position lives in NodeStorage.user_data (the flow domain's
    ///    editor-metadata slot; LFGR will persist it in P3).
    ///  - detach/attach ride FlowGraph::extractNode / insertNodeAt — the
    ///    id-stability contract for undo.
    class FlowGraphView final : public lux::graphkit::IGraphView
    {
    public:
        FlowGraphView(lux::flowforge::FlowGraph& graph,
                      lux::flowforge::NodeRegistry& registry)
            : graph_(&graph), registry_(&registry)
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

        /// Resolves a pin ref to the live flowforge pin (nullptr if stale).
        lux::flowforge::Pin* pinAt(lux::graphkit::GraphPinRef ref) const;

    private:
        lux::flowforge::FlowGraph*    graph_;
        lux::flowforge::NodeRegistry* registry_;
    };

    /// IGraphSchema adapter: connection legality DELEGATES to the existing
    /// Pin::canLink (flowforge's connection-rule SSOT); the palette is a 1:1
    /// forward of NodeRegistry's records; node actions surface SequenceNode's
    /// variable exec-out pins; unconnected data-in constants are edited inline
    /// via ConstantEditor (parity with the retired NodePainter).
    class FlowSchema final : public lux::graphkit::IGraphSchema
    {
    public:
        FlowSchema(lux::flowforge::FlowGraph& graph, lux::flowforge::NodeRegistry& registry,
                   FlowGraphView& view)
            : graph_(&graph), registry_(&registry), view_(&view)
        {
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
        std::span<const lux::graphkit::NodeAction>
             nodeActions(lux::graphkit::GraphNodeRef node,
                         const lux::graphkit::IGraphView& view) const override;
        void runNodeAction(lux::graphkit::GraphNodeRef node, std::string_view action_id,
                           lux::graphkit::IGraphView& view) override;
        void drawNodeBody(lux::graphkit::GraphNodeRef node, lux::graphkit::IGraphView& view,
                          lux::graphkit::DeferredPopupQueue& popups) override;

    private:
        lux::flowforge::FlowGraph*    graph_;
        lux::flowforge::NodeRegistry* registry_;
        FlowGraphView*                view_;

        mutable std::vector<lux::graphkit::NodeTemplate> palette_cache_;
        mutable std::vector<lux::graphkit::NodeAction>   actions_cache_;
    };

    class FlowGraphPanel : public lux::ui::Panel
    {
    public:
        explicit FlowGraphPanel(std::string title);
        ~FlowGraphPanel() override;

        /// The graph the MLIR compiler consumes (generateIR reads the SSOT
        /// directly — never through the GraphKit port).
        const lux::flowforge::FlowGraph& graph() const { return graph_; }

        /// Wire the project asset index + asset manager (for "Save as
        /// Asset" / openAsset) + process DomainEvents (to announce a committed
        /// saved graph). registry/events borrowed (owned by host); manager shared.
        void setAssetServices(AssetRegistry* registry,
                              std::shared_ptr<lux::asset::AssetManager> manager,
                              lux::events::DomainEvents* events);

        /// Open a FLOW_GRAPH asset in the editor (double-click in the asset
        /// browser). The persisted graph carries node positions, so the
        /// layout is restored.
        void openAsset(const lux::asset::asset_id_t& id);

        /// Background precompile on save: called with the saved asset's id right
        /// after a successful Save-as-Asset, so the AOT cache is warm before
        /// the next Play. Wired by the host to FlowForgeCompilerService.
        using PrecompileHook =
            std::function<void(lux::asset::AssetManager&,
                               const lux::asset::asset_id_t&)>;
        void setPrecompileHook(PrecompileHook hook) { precompile_hook_ = std::move(hook); }

    private:
        void paint() override;

        /// Seeds the default "Hello World" graph (Start -> Print(42) -> Return;
        /// Print = NATIVE_FUNC_CALL of the hand-built `lux_flow_print(int)`
        /// signature) and rebinds the editor (full reset). Called at
        /// construction and by the toolbar's "New" button — a new FlowGraph
        /// always starts as a runnable Hello World.
        void buildHelloWorldGraph();

        /// Serialize the current graph into a FlowGraphAsset and persist it
        /// under <project>/FlowGraphs/<name>.luxasset + the modal name popup.
        bool saveAsAsset(const std::string& name, std::string* err);
        void drawSavePopup();

        lux::flowforge::FlowGraph     graph_;
        // The PROCESS-WIDE registry: the graph serializer re-instantiates
        // native-call nodes by creator name through NodeRegistry::global(),
        // so the panel registers its natives there (not in a private copy).
        lux::flowforge::NodeRegistry& registry_ = lux::flowforge::NodeRegistry::global();
        FlowGraphView                 view_{ graph_, registry_ };
        FlowSchema                    schema_{ graph_, registry_, view_ };
        lux::graphkit::GraphEditor    editor_;

        // Asset services (borrowed/shared; may be null when no project).
        AssetRegistry*                             asset_registry_{ nullptr };
        std::shared_ptr<lux::asset::AssetManager>  asset_manager_;
        lux::events::DomainEvents*                 events_{ nullptr };
        PrecompileHook                             precompile_hook_;

        bool        save_popup_open_{ false };
        std::string save_name_;
        std::string save_status_;
        std::string status_;

#if LUX_FLOWFORGE_HAS_MLIR
        // Folded-in CompilerPanel: FlowGraph -> FlowForge dialect IR text,
        // plus the run-button path (JIT execute + captured native output).
        std::string                                  ir_buffer_;
        std::string                                  run_status_;
        std::unique_ptr<lux::flowforge::IRContext>   ir_context_;
        std::unique_ptr<lux::flowforge::MLIRBuilder> mlir_builder_;
#endif
    };

} // namespace lux::editor
