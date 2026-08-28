#pragma once
// =============================================================================
//  GraphEditor.hpp — THE single imgui-node-editor integration (GraphKit's
//  core). Hosts panels embed one GraphEditor + inject a domain IGraphView +
//  IGraphSchema; everything canvas-shaped lives here ONCE:
//
//   * node rendering (title from the read core + schema drawNodeBody + pin
//     rows styled via schema pinStyle)
//   * link rendering derived from forEachLink every frame
//   * drag-to-connect (schema canConnect verdict + reject tooltip) with the
//     cap-aware replace handled by the command stack
//   * delete handling (links + nodes, recorded as inverse intents)
//   * node drag write-back to the domain's canvas position (one undoable move
//     per completed drag gesture)
//   * the right-click palette (category-grouped fuzzy SearchPopupElement) and
//     the node context menu (schema nodeActions) — both drawn OUTSIDE the
//     canvas (the documented imgui-node-editor popup gotcha)
//   * Ctrl+Z / Ctrl+Y over the inverse-intent GraphCommandStack
//   * the fallback grid layout for unplaced nodes
//
//  imgui-node-editor's own settings file is DISABLED (NodeEditor.json is never
//  written): node positions live in the DOMAIN's editor metadata, read/written
//  through the port — the graph stays the single source of truth.
//
//  REBIND CONTRACT: bind() retargets this editor at another graph with FULL
//  RESET semantics — the id bimap, drag state, palette state and the command
//  stack are cleared (recorded intents reference the previous graph's ids).
//  Hosts rebind on document switch (double-click-reopen); a future subgraph
//  breadcrumb stack rides this same contract with zero editor change.
// =============================================================================

#include <memory>
#include <vector>

#include "GraphCommandStack.hpp"
#include "GraphTypes.hpp"
#include "IGraphSchema.hpp"
#include "IGraphView.hpp"

namespace lux::editor::node_graph
{
    class LUX_NODE_GRAPH_EDITOR_PUBLIC GraphEditor
    {
    public:
        GraphEditor();
        ~GraphEditor();

        GraphEditor(const GraphEditor&) = delete;
        GraphEditor& operator=(const GraphEditor&) = delete;

        /// Retargets the editor at a (view, schema) pair. FULL RESET — see the
        /// header contract. Both may be null to unbind.
        void bind(IGraphView* view, IGraphSchema* schema);

        /// Draws the canvas + popups. Call from the host panel's paint(),
        /// inside a normal ImGui window. No-op while unbound.
        void paint(const char* canvas_id);

        GraphCommandStack& commands();

        /// Undo/redo THROUGH the editor (not commands().undo() directly): a
        /// history jump can re-create nodes, so the editor must also reset its
        /// id mappings and re-sync positions. The internal Ctrl+Z/Y path uses
        /// these same entry points.
        bool undo();
        bool redo();

        /// Drains the popup requests raised by IGraphSchema::drawNodeBody this
        /// frame; the HOST draws the floating editors in panel space.
        [[nodiscard]] std::vector<DeferredPopupRequest> takeDeferredPopups();

        /// 拓扑锁(R3:材质实例模式)。锁定时画布转为「结构只读」:调色板
        /// 不再弹出、连线创建/删除被拒(红框反馈)、节点删除被拒、节点拖动
        /// 回弹(位置属于宿主文档,锁定态不得写)、Ctrl+Z/Y 与 undo()/redo()
        /// no-op。**保留**:平移/缩放/框选/悬停 + schema 的节点体控件(值
        /// 编辑面由 schema 自行决定 —— 锁的是拓扑,不是节点体)。
        /// bind() 不重置本标志:模式归宿主管,rebind 后由宿主重设。
        void setTopologyLocked(bool locked);

        /// Re-runs the fallback grid layout for unplaced nodes on the next
        /// paint (hosts call this after decoding an asset without positions).
        void requestLayout();

        /// Frames the whole graph on the next paint.
        void navigateToContent();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace lux::editor::node_graph
