#include <lux/engine/editor/framework/graphkit/GraphEditor.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <utility>

#include <imgui.h>
#include <imgui_node_editor.h>

#include <lux/engine/editor/framework/graphkit/GraphLayout.hpp>
#include <lux/engine/editor/framework/graphkit/detail/PinIdBimap.hpp>
#include <lux/engine/ui/SearchPopupElement.hpp>

namespace ed = ax::NodeEditor;

namespace lux::graphkit
{
    namespace
    {
        constexpr float kPinIconSize  = 12.0f;
        constexpr float kIconLabelGap = 6.0f;
        constexpr float kColumnGap    = 24.0f;
        constexpr float kDragEpsilon  = 0.5f;   ///< canvas units; below = not a move

        ImU32 toImColor(std::uint32_t packed) { return static_cast<ImU32>(packed); }

        /// Blueprint-style circle pin dot: filled = connected, dark inner +
        /// colored ring = free (lifted from MaterialGraphPanel's drawPinIcon).
        void drawPinIcon(ImU32 color, bool filled)
        {
            const ImVec2 cursor = ImGui::GetCursorScreenPos();
            const ImVec2 center{ cursor.x + kPinIconSize * 0.5f,
                                 cursor.y + ImGui::GetTextLineHeight() * 0.5f };
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float r = kPinIconSize * 0.5f - 1.0f;
            if (filled)
            {
                dl->AddCircleFilled(center, r, color, 12);
            }
            else
            {
                dl->AddCircleFilled(center, r, IM_COL32(32, 32, 32, 255), 12);
                dl->AddCircle(center, r, color, 12, 1.6f);
            }
            ImGui::Dummy(ImVec2(kPinIconSize, ImGui::GetTextLineHeight()));
        }

        void textUnformatted(std::string_view s)
        {
            ImGui::TextUnformatted(s.data(), s.data() + s.size());
        }

        float textWidth(std::string_view s)
        {
            return ImGui::CalcTextSize(s.data(), s.data() + s.size()).x;
        }
    } // namespace

    struct GraphEditor::Impl
    {
        ed::EditorContext*  ctx    = nullptr;
        IGraphView*         view   = nullptr;
        IGraphSchema*       schema = nullptr;

        GraphCommandStack   commands;
        detail::PinIdBimap  bimap;
        DeferredPopupQueue  popups;

        // Palette popup (drawn AFTER the canvas ends; SearchPopupElement
        // documents that requirement itself).
        lux::ui::SearchPopupElement palette{ "GraphKitPalette" };
        std::vector<std::string>    palette_template_ids; ///< select index -> template id
        bool                        open_palette = false;
        GraphVec2                   palette_spawn_pos{};

        // Node context menu (schema nodeActions), also deferred out of canvas.
        bool         open_node_menu = false;
        GraphNodeRef menu_node{};

        bool needs_layout   = true;
        bool needs_pos_sync = true;
        bool want_navigate  = false;

        // 拓扑锁(R3):结构只读 —— 见 GraphEditor::setTopologyLocked 的契约。
        bool topology_locked = false;

        // Live drag tracking: node id -> position at drag start (for the one
        // undoable move recorded when the gesture ends).
        std::unordered_map<node_id, GraphVec2> drag_start;

        // ---- helpers --------------------------------------------------------
        void afterHistoryJump()
        {
            // Wholesale eager purge: undo/redo can re-create nodes (and
            // flowforge recycles ids) — never let a stale mapping alias.
            bimap.clear();
            needs_pos_sync = true;
        }

        void syncPositionsFromView()
        {
            view->forEachNode(
                [&](GraphNodeRef node, std::string_view)
                {
                    if (const auto pos = view->nodePos(node))
                    {
                        ed::SetNodePosition(ed::NodeId(bimap.nodeEdId(node)),
                                            ImVec2(pos->x, pos->y));
                    }
                }
            );
        }

        void drawNode(GraphNodeRef node, std::string_view title)
        {
            const NodeStyleDesc style = schema->nodeStyle(node, *view);
            const ed::NodeId    ed_id{ bimap.nodeEdId(node) };

            ed::BeginNode(ed_id);
            ImGui::PushID(static_cast<int>(node.id));

            textUnformatted(title.empty() ? std::string_view{ "(node)" } : title);
            // Colored header band geometry (drawn AFTER EndNode on the node's
            // background layer — MatNodeBuilder technique).
            float header_bottom_y = 0.0f;
            if (style.draw_header)
            {
                header_bottom_y = ImGui::GetItemRectMax().y
                                + ImGui::GetStyle().ItemSpacing.y;
                ImGui::Dummy(ImVec2(0.0f, 2.0f)); // gap before body/pins
            }
            else
            {
                ImGui::Spacing();
            }

            schema->drawNodeBody(node, *view, popups);

            const std::uint32_t in_count  = view->pinCount(node, EPinSide::INPUT);
            const std::uint32_t out_count = view->pinCount(node, EPinSide::OUTPUT);

            // Right-align the output column: pre-measure the widest label so
            // every output's dot sits flush at the node's right edge.
            float out_col_w = 0.0f;
            for (std::uint32_t i = 0; i < out_count; ++i)
            {
                const GraphPinView pv = view->pin(node, EPinSide::OUTPUT, i);
                out_col_w = std::max(out_col_w,
                                     textWidth(pv.name) + kIconLabelGap + kPinIconSize);
            }

            if (in_count > 0)
            {
                ImGui::BeginGroup();
                for (std::uint32_t i = 0; i < in_count; ++i)
                {
                    const GraphPinRef  ref{ node, EPinSide::INPUT, i };
                    const GraphPinView pv = view->pin(node, EPinSide::INPUT, i);
                    const ImU32 color = toImColor(schema->pinStyle(pv.type).color);

                    ed::BeginPin(ed::PinId(bimap.pinEdId(ref)), ed::PinKind::Input);
                    ed::PinPivotAlignment(ImVec2(0.0f, 0.5f)); // link attaches at the LEFT edge
                    ed::PinPivotSize(ImVec2(0.0f, 0.0f));
                    drawPinIcon(color, pv.has_link);
                    if (!pv.name.empty())
                    {
                        ImGui::SameLine(0.0f, kIconLabelGap);
                        textUnformatted(pv.name);
                    }
                    ed::EndPin();
                }
                ImGui::EndGroup();
            }
            if (in_count > 0 && out_count > 0)
            {
                ImGui::SameLine(0.0f, kColumnGap);
            }
            if (out_count > 0)
            {
                ImGui::BeginGroup();
                for (std::uint32_t i = 0; i < out_count; ++i)
                {
                    const GraphPinRef  ref{ node, EPinSide::OUTPUT, i };
                    const GraphPinView pv = view->pin(node, EPinSide::OUTPUT, i);
                    const ImU32 color = toImColor(schema->pinStyle(pv.type).color);

                    ed::BeginPin(ed::PinId(bimap.pinEdId(ref)), ed::PinKind::Output);
                    ed::PinPivotAlignment(ImVec2(1.0f, 0.5f)); // link attaches at the RIGHT edge
                    ed::PinPivotSize(ImVec2(0.0f, 0.0f));
                    ImGui::BeginGroup();
                    const float used =
                        textWidth(pv.name) + kIconLabelGap + kPinIconSize;
                    if (out_col_w - used > 0.0f)
                    {
                        ImGui::Dummy(ImVec2(out_col_w - used, 1.0f));
                        ImGui::SameLine(0.0f, 0.0f);
                    }
                    if (!pv.name.empty())
                    {
                        textUnformatted(pv.name);
                        ImGui::SameLine(0.0f, kIconLabelGap);
                    }
                    drawPinIcon(color, pv.has_link);
                    ImGui::EndGroup();
                    ed::EndPin();
                }
                ImGui::EndGroup();
            }

            ImGui::PopID();
            ed::EndNode();

            // Paint the header band on the background layer, clipped to the
            // node's rounded top (ports MatNodeBuilder::end verbatim).
            if (style.draw_header && ImGui::IsItemVisible())
            {
                const ImVec2 n_min = ImGui::GetItemRectMin();
                const ImVec2 n_max = ImGui::GetItemRectMax();
                if (ImDrawList* bg = ed::GetNodeBackgroundDrawList(ed_id))
                {
                    const float  rounding = ed::GetStyle().NodeRounding;
                    const float  hbw      = ed::GetStyle().NodeBorderWidth * 0.5f;
                    const ImVec2 h_min(n_min.x + hbw, n_min.y + hbw);
                    const ImVec2 h_max(n_max.x - hbw, header_bottom_y);
                    const int    a   = static_cast<int>(255 * ImGui::GetStyle().Alpha);
                    const ImU32  col = (toImColor(style.header_color)
                                        & IM_COL32(255, 255, 255, 0))
                                     | IM_COL32(0, 0, 0, a);
                    bg->AddRectFilled(h_min, h_max, col, rounding,
                                      ImDrawFlags_RoundCornersTop);
                    bg->AddLine(ImVec2(h_min.x, h_max.y - 0.5f),
                                ImVec2(h_max.x, h_max.y - 0.5f),
                                IM_COL32(255, 255, 255, 40), 1.0f);
                }
            }
        }

        void handleCreate()
        {
            if (topology_locked)
            {
                // 锁定态:一律拒绝新链(灰框 + 说明),Begin/End 仍要配对。
                if (ed::BeginCreate())
                {
                    ed::PinId a, b;
                    if (ed::QueryNewLink(&a, &b) && a && b)
                    {
                        ed::RejectNewItem(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), 2.0f);
                        ed::Suspend();
                        ImGui::SetTooltip("%s", "拓扑锁定(实例模式):结构属于根材质");
                        ed::Resume();
                    }
                }
                ed::EndCreate();
                return;
            }
            if (ed::BeginCreate())
            {
                ed::PinId a, b;
                if (ed::QueryNewLink(&a, &b) && a && b && a != b)
                {
                    const GraphPinRef ra = bimap.pinFromEdId(static_cast<std::uintptr_t>(a.Get()));
                    const GraphPinRef rb = bimap.pinFromEdId(static_cast<std::uintptr_t>(b.Get()));
                    if (ra.valid() && rb.valid() && ra.node != rb.node && ra.side != rb.side)
                    {
                        // Orient: from = the OUTPUT pin, to = the INPUT pin.
                        const GraphPinRef from = (ra.side == EPinSide::OUTPUT) ? ra : rb;
                        const GraphPinRef to   = (ra.side == EPinSide::OUTPUT) ? rb : ra;

                        const ConnectResult verdict = schema->canConnect(from, to, *view);
                        if (!verdict.allowed)
                        {
                            ed::RejectNewItem(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), 2.0f);
                            if (!verdict.reason.empty())
                            {
                                ed::Suspend();
                                ImGui::SetTooltip("%s", verdict.reason.c_str());
                                ed::Resume();
                            }
                        }
                        else if (ed::AcceptNewItem())
                        {
                            if (commands.doConnect(from, to))
                            {
                                schema->onPinConnected(from, to, *view);
                            }
                        }
                    }
                    else
                    {
                        ed::RejectNewItem();
                    }
                }
            }
            ed::EndCreate();
        }

        void handleDelete()
        {
            if (topology_locked)
            {
                // 锁定态:链/节点删除一律拒绝,Begin/End 仍要配对。
                if (ed::BeginDelete())
                {
                    ed::LinkId l;
                    while (ed::QueryDeletedLink(&l))
                        ed::RejectDeletedItem();
                    ed::NodeId n;
                    while (ed::QueryDeletedNode(&n))
                        ed::RejectDeletedItem();
                }
                ed::EndDelete();
                return;
            }
            if (ed::BeginDelete())
            {
                ed::LinkId link_id;
                while (ed::QueryDeletedLink(&link_id))
                {
                    GraphPinRef from, to;
                    if (bimap.linkFromEdId(static_cast<std::uintptr_t>(link_id.Get()), from, to))
                    {
                        if (ed::AcceptDeletedItem())
                        {
                            commands.doDisconnect(from, to);
                            schema->onPinDisconnected(from, to, *view);
                        }
                    }
                    else
                    {
                        ed::RejectDeletedItem();
                    }
                }

                ed::NodeId node_id_;
                while (ed::QueryDeletedNode(&node_id_))
                {
                    const GraphNodeRef node =
                        bimap.nodeFromEdId(static_cast<std::uintptr_t>(node_id_.Get()));
                    if (node.valid() && ed::AcceptDeletedItem())
                    {
                        commands.doRemoveNode(node);
                        bimap.purgeNode(node.id);
                    }
                }
            }
            ed::EndDelete();
        }

        /// Live position write-back while dragging + ONE undoable move per
        /// node when the gesture ends.
        void handleNodeDrag()
        {
            if (topology_locked)
            {
                // 锁定态:位置属于宿主文档(实例不得写根图的 ui_pos)——
                // 用户拖动一律回弹到存储位置,不入账。
                view->forEachNode(
                    [&](GraphNodeRef node, std::string_view)
                    {
                        const auto stored = view->nodePos(node);
                        if (!stored)
                            return;
                        const ed::NodeId nid(bimap.nodeEdId(node));
                        const ImVec2     p = ed::GetNodePosition(nid);
                        if (std::fabs(p.x - stored->x) > kDragEpsilon ||
                            std::fabs(p.y - stored->y) > kDragEpsilon)
                            ed::SetNodePosition(nid, ImVec2(stored->x, stored->y));
                    });
                drag_start.clear();
                return;
            }
            view->forEachNode(
                [&](GraphNodeRef node, std::string_view)
                {
                    const auto stored = view->nodePos(node);
                    if (!stored)
                    {
                        return;
                    }
                    const ImVec2 p = ed::GetNodePosition(ed::NodeId(bimap.nodeEdId(node)));
                    if (std::fabs(p.x - stored->x) > kDragEpsilon ||
                        std::fabs(p.y - stored->y) > kDragEpsilon)
                    {
                        drag_start.emplace(node.id, *stored); // keep the FIRST (gesture start)
                        view->setNodePos(node, GraphVec2{ p.x, p.y });
                    }
                }
            );

            if (!drag_start.empty() && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                commands.beginTransaction("move");
                for (const auto& [id, start] : drag_start)
                {
                    const GraphNodeRef node{ id };
                    const GraphVec2 now = view->nodePos(node).value_or(start);
                    if (std::fabs(now.x - start.x) > kDragEpsilon ||
                        std::fabs(now.y - start.y) > kDragEpsilon)
                    {
                        commands.doMoveNode(node, start, now);
                    }
                }
                commands.commitTransaction();
                drag_start.clear();
            }
        }

        void queryContextMenus()
        {
            ed::Suspend();
            if (ed::ShowBackgroundContextMenu())
            {
                if (!topology_locked)   // 锁定态:不弹加节点的调色板
                {
                    open_palette = true;
                    const ImVec2 canvas = ed::ScreenToCanvas(ImGui::GetMousePos());
                    palette_spawn_pos = GraphVec2{ canvas.x, canvas.y };
                }
            }
            ed::NodeId menu_ed_node;
            if (ed::ShowNodeContextMenu(&menu_ed_node))
            {
                menu_node = bimap.nodeFromEdId(static_cast<std::uintptr_t>(menu_ed_node.Get()));
                open_node_menu = menu_node.valid();
            }
            ed::Resume();
        }

        /// Popups — drawn AFTER ed::End() in normal panel space.
        void drawPopups()
        {
            if (open_palette)
            {
                open_palette = false;
                std::vector<std::string> names;
                palette_template_ids.clear();
                for (const NodeTemplate& t : schema->palette())
                {
                    names.push_back(t.category.empty() ? t.display_name
                                                       : t.category + " / " + t.display_name);
                    palette_template_ids.push_back(t.id);
                }
                palette.setItems(names);
                palette.setHint("add node...");
                palette.setOnSelect(
                    [this](std::uint32_t index)
                    {
                        if (index < palette_template_ids.size())
                        {
                            commands.doAddNode(palette_template_ids[index], palette_spawn_pos);
                            needs_pos_sync = true;
                        }
                    }
                );
                palette.open();
            }
            palette.draw();

            if (open_node_menu)
            {
                ImGui::OpenPopup("gk_node_actions");
                open_node_menu = false;
            }
            if (ImGui::BeginPopup("gk_node_actions"))
            {
                const auto actions = schema->nodeActions(menu_node, *view);
                if (actions.empty())
                {
                    ImGui::TextDisabled("(no actions)");
                }
                for (const NodeAction& action : actions)
                {
                    if (ImGui::MenuItem(action.label.c_str()))
                    {
                        schema->runNodeAction(menu_node, action.id, *view);
                        // Action may have rebuilt the node's pins; drop its
                        // mappings eagerly (the eager-purge rule).
                        bimap.purgeNode(menu_node.id);
                    }
                }
                ImGui::EndPopup();
            }
        }

        void handleShortcuts()
        {
            if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows | ImGuiFocusedFlags_RootWindow))
            {
                return;
            }
            const ImGuiIO& io = ImGui::GetIO();
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false))
            {
                undoThroughEditor();
            }
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false))
            {
                redoThroughEditor();
            }
        }

        bool undoThroughEditor()
        {
            if (topology_locked)
                return false;   // 锁定态:历史回放会改结构,一并封死
            if (!commands.undo())
            {
                return false;
            }
            afterHistoryJump();
            return true;
        }

        bool redoThroughEditor()
        {
            if (topology_locked)
                return false;
            if (!commands.redo())
            {
                return false;
            }
            afterHistoryJump();
            return true;
        }
    };

    // ---- public surface ----------------------------------------------------------

    GraphEditor::GraphEditor()
        : impl_(std::make_unique<Impl>())
    {
        ed::Config config;
        // Node positions are DOMAIN data (editor-metadata block), read/written
        // through the port — never imgui-node-editor's own settings file
        // (NodeEditor.json stays unwritten, per project rule).
        config.SettingsFile = nullptr;
        impl_->ctx = ed::CreateEditor(&config);

        // Blueprint-style look (lifted from MaterialGraphPanel): rounded nodes
        // + subtle border; small top padding so the colored header band butts
        // flush against the rounded top.
        ed::SetCurrentEditor(impl_->ctx);
        {
            ed::Style& st = ed::GetStyle();
            st.NodeRounding            = 6.0f;
            st.NodeBorderWidth         = 1.5f;
            st.HoveredNodeBorderWidth  = 2.5f;
            st.SelectedNodeBorderWidth = 3.0f;
            st.PinRounding             = 3.0f;
            st.NodePadding             = ImVec4(8.0f, 4.0f, 8.0f, 8.0f);
            st.Colors[ed::StyleColor_NodeBg]     = ImColor(36, 36, 40, 220);
            st.Colors[ed::StyleColor_NodeBorder] = ImColor(255, 255, 255, 70);
        }
        ed::SetCurrentEditor(nullptr);
    }

    GraphEditor::~GraphEditor()
    {
        if (impl_ && impl_->ctx)
        {
            ed::DestroyEditor(impl_->ctx);
        }
    }

    void GraphEditor::bind(IGraphView* view, IGraphSchema* schema)
    {
        Impl& s = *impl_;
        s.view   = view;
        s.schema = schema;
        // FULL RESET (the rebind contract).
        s.commands.bind(view);
        s.bimap.clear();
        static_cast<void>(s.popups.take()); // drop any stale requests
        s.drag_start.clear();
        s.palette_template_ids.clear();
        s.open_palette   = false;
        s.open_node_menu = false;
        s.menu_node      = {};
        s.needs_layout   = true;
        s.needs_pos_sync = true;
    }

    void GraphEditor::paint(const char* canvas_id)
    {
        Impl& s = *impl_;
        if (!s.view || !s.schema || !s.ctx)
        {
            return;
        }

        ed::SetCurrentEditor(s.ctx);
        ed::Begin(canvas_id, ImVec2(0.0f, 0.0f));

        if (s.needs_layout)
        {
            layoutUnplacedNodes(*s.view);
            s.needs_layout   = false;
            s.needs_pos_sync = true;
        }
        if (s.needs_pos_sync)
        {
            s.syncPositionsFromView();
            s.needs_pos_sync = false;
        }

        s.view->forEachNode(
            [&](GraphNodeRef node, std::string_view title) { s.drawNode(node, title); });

        s.view->forEachLink(
            [&](GraphLinkView link)
            {
                ed::Link(ed::LinkId(s.bimap.linkEdId(link.from, link.to)),
                         ed::PinId(s.bimap.pinEdId(link.from)),
                         ed::PinId(s.bimap.pinEdId(link.to)));
            }
        );

        s.handleCreate();
        s.handleDelete();
        s.handleNodeDrag();
        s.queryContextMenus();

        if (s.want_navigate)
        {
            ed::NavigateToContent();
            s.want_navigate = false;
        }

        ed::End();
        ed::SetCurrentEditor(nullptr);

        // Out-of-canvas phase: popups + shortcuts (normal panel space).
        s.drawPopups();
        s.handleShortcuts();
    }

    GraphCommandStack& GraphEditor::commands()
    {
        return impl_->commands;
    }

    bool GraphEditor::undo()
    {
        return impl_->undoThroughEditor();
    }

    bool GraphEditor::redo()
    {
        return impl_->redoThroughEditor();
    }

    std::vector<DeferredPopupRequest> GraphEditor::takeDeferredPopups()
    {
        return impl_->popups.take();
    }

    void GraphEditor::setTopologyLocked(bool locked)
    {
        impl_->topology_locked = locked;
    }

    void GraphEditor::requestLayout()
    {
        impl_->needs_layout = true;
    }

    void GraphEditor::navigateToContent()
    {
        impl_->want_navigate = true;
    }

} // namespace lux::graphkit
