#include <lux/engine/editor/node_graph/DefaultImGuiNodeGraphRenderer.hpp>

#include <imgui.h>
#include <imgui_node_editor.h>

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ed = ax::NodeEditor;

namespace lux::editor::node_graph
{
    namespace
    {
        struct LinkKey final
        {
            graph::PinId from;
            graph::PinId to;
            [[nodiscard]] bool operator==(const LinkKey&) const noexcept = default;
        };

        struct LinkKeyHash final
        {
            [[nodiscard]] std::size_t operator()(const LinkKey& key) const noexcept
            {
                auto result = std::hash<graph::PinId>{}(key.from);
                result ^= std::hash<graph::PinId>{}(key.to) + 0x9E3779B9U + (result << 6U) + (result >> 2U);
                return result;
            }
        };

        [[nodiscard]] ed::NodeId nodeId(graph::NodeId value) noexcept
        {
            return ed::NodeId{static_cast<std::uintptr_t>(value.value)};
        }

        [[nodiscard]] ed::PinId pinId(graph::PinId value) noexcept
        {
            return ed::PinId{static_cast<std::uintptr_t>(value.value)};
        }

        [[nodiscard]] graph::NodeId selectedNode() noexcept
        {
            ed::NodeId selected;
            return ed::GetSelectedNodes(&selected, 1) == 0 ? graph::NodeId{} :
                                                            graph::NodeId{static_cast<std::uint64_t>(selected.Get())};
        }
    } // namespace

    struct DefaultImGuiNodeGraphRenderer::Impl final
    {
        Impl()
        {
            ed::Config config;
            config.SettingsFile = nullptr;
            context = ed::CreateEditor(&config);
        }

        ~Impl()
        {
            ed::DestroyEditor(context);
        }

        ed::EditorContext* context{};
        std::unordered_set<graph::NodeId> initialized_nodes;
        std::unordered_map<LinkKey, std::uintptr_t, LinkKeyHash> link_ids;
        std::unordered_map<std::uintptr_t, LinkKey> links_by_id;
        std::uintptr_t next_link_id{1U};
        bool palette_open{};
        ImVec2 palette_position{};

        [[nodiscard]] std::uintptr_t linkId(LinkKey key)
        {
            const auto found = link_ids.find(key);
            if (found != link_ids.end())
                return found->second;
            const auto id = next_link_id++;
            link_ids.emplace(key, id);
            links_by_id.emplace(id, key);
            return id;
        }
    };

    DefaultImGuiNodeGraphRenderer::DefaultImGuiNodeGraphRenderer() : impl_(std::make_unique<Impl>())
    {
    }

    DefaultImGuiNodeGraphRenderer::~DefaultImGuiNodeGraphRenderer() = default;

    void DefaultImGuiNodeGraphRenderer::draw(
        const char* canvas_id,
        const GraphRenderProtocol& protocol,
        IGraphIntentSink& sink
    )
    {
        ed::SetCurrentEditor(impl_->context);
        ed::Begin(canvas_id);
        if (selectedNode() != protocol.selected)
        {
            ed::ClearSelection();
            if (protocol.selected.valid())
                ed::SelectNode(nodeId(protocol.selected));
        }

        for (const auto& node : protocol.topology.nodes())
        {
            ed::BeginNode(nodeId(node.id));
            const auto presentation = protocol.presentation.node(node.id);
            ImGui::TextUnformatted(presentation.title.data(), presentation.title.data() + presentation.title.size());
            for (const auto& pin : protocol.topology.pins())
            {
                if (pin.owner != node.id)
                    continue;
                const auto pin_presentation = protocol.presentation.pin(pin.id);
                const auto kind = pin.direction == graph::EPinDirection::INPUT ? ed::PinKind::Input :
                                                                                 ed::PinKind::Output;
                ed::BeginPin(pinId(pin.id), kind);
                ImGui::TextUnformatted(
                    pin_presentation.label.data(),
                    pin_presentation.label.data() + pin_presentation.label.size()
                );
                ed::EndPin();
            }
            ed::EndNode();

            if (impl_->initialized_nodes.insert(node.id).second)
            {
                if (const auto* layout = protocol.layout.find(node.id); layout != nullptr && layout->placed)
                    ed::SetNodePosition(nodeId(node.id), ImVec2{layout->x, layout->y});
            }
        }

        for (const auto& link : protocol.topology.links())
        {
            const LinkKey key{link.from, link.to};
            ed::Link(ed::LinkId{impl_->linkId(key)}, pinId(link.from), pinId(link.to));
        }

        if (!protocol.topology_locked && ed::BeginCreate())
        {
            ed::PinId first;
            ed::PinId second;
            if (ed::QueryNewLink(&first, &second) && first && second && ed::AcceptNewItem())
            {
                sink.emit(ConnectIntent{
                    graph::PinId{static_cast<std::uint64_t>(first.Get())},
                    graph::PinId{static_cast<std::uint64_t>(second.Get())}
                });
            }
            ed::EndCreate();
        }

        if (!protocol.topology_locked && ed::BeginDelete())
        {
            ed::LinkId link;
            while (ed::QueryDeletedLink(&link))
            {
                if (!ed::AcceptDeletedItem())
                    continue;
                const auto found = impl_->links_by_id.find(static_cast<std::uintptr_t>(link.Get()));
                if (found != impl_->links_by_id.end())
                    sink.emit(DisconnectIntent{found->second.from, found->second.to});
            }
            ed::NodeId node;
            while (ed::QueryDeletedNode(&node))
            {
                if (ed::AcceptDeletedItem())
                    sink.emit(RemoveNodeIntent{graph::NodeId{static_cast<std::uint64_t>(node.Get())}});
            }
            ed::EndDelete();
        }

        if (!protocol.topology_locked && ed::ShowBackgroundContextMenu())
        {
            impl_->palette_open = true;
            impl_->palette_position = ed::ScreenToCanvas(ImGui::GetMousePos());
        }

        ed::End();

        const auto selected = selectedNode();
        if (selected != protocol.selected)
            sink.emit(SelectNodeIntent{selected, false});

        for (const auto& node : protocol.topology.nodes())
        {
            const auto position = ed::GetNodePosition(nodeId(node.id));
            const auto* previous = protocol.layout.find(node.id);
            const bool changed = previous == nullptr || !previous->placed || previous->x != position.x ||
                previous->y != position.y;
            if (changed)
                sink.emit(MoveNodeIntent{node.id, graph::GraphNodeLayout{position.x, position.y, true}});
        }

        if (impl_->palette_open)
            ImGui::OpenPopup("Add Graph Node");
        if (ImGui::BeginPopup("Add Graph Node"))
        {
            impl_->palette_open = false;
            for (const auto& entry : protocol.presentation.palette())
            {
                if (ImGui::MenuItem(entry.display_name.c_str()))
                {
                    sink.emit(AddNodeIntent{
                        entry.type,
                        graph::GraphNodeLayout{
                            impl_->palette_position.x,
                            impl_->palette_position.y,
                            true
                        }
                    });
                }
            }
            ImGui::EndPopup();
        }

        ed::SetCurrentEditor(nullptr);
    }
} // namespace lux::editor::node_graph
