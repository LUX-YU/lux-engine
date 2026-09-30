#include <lux/engine/editor/widgets/GraphCanvas.hpp>
#include <imgui.h>
#include <algorithm>
namespace lux::editor::widgets
{
    namespace canvas = ax::NodeEditor;
    GraphCanvas::GraphCanvas(lux::ui::Element& parent, lux::ui::ElementId id)
        : Element(parent, std::move(id)), canvas_(createCanvas())
    {
        setStretch({1, 1});
    }
    void GraphCanvas::setGraph(std::vector<CanvasNode> nodes, std::vector<CanvasLink> links)
    {
        nodes_ = std::move(nodes);
        links_ = std::move(links);
        CanvasScope scope(canvas_.get());
        for (std::size_t i{}; i < nodes_.size(); ++i)
        {
            auto& node = nodes_[i];
            if (!node.placed)
                node.position = {24.F + (i % 4) * 220.F, 24.F + (i / 4) * 150.F};
            canvas::SetNodePosition(ids_.node(node.id), {node.position.x, node.position.y});
        }
        selected_.resize(nodes_.size());
        selection_.reserve(nodes_.size());
        movement_.nodes.reserve(nodes_.size());
    }
    void GraphCanvas::publish(CanvasEdit value) noexcept
    {
        delivery_ = emit(edited, value);
    }
    void GraphCanvas::finishEdit(bool cancel) noexcept
    {
        if (!moving_)
            return;
        publish({movement_, false, !cancel, cancel});
        if (cancel)
        {
            CanvasScope scope(canvas_.get());
            for (const auto& node : nodes_)
                canvas::SetNodePosition(ids_.node(node.id), {node.position.x, node.position.y});
        }
        moving_ = false;
    }
    void GraphCanvas::draw() noexcept
    {
        CanvasScope scope(canvas_.get());
        canvas::Begin("graph", {rect().size.width, rect().size.height});
        for (const auto& node : nodes_)
        {
            canvas::BeginNode(ids_.node(node.id));
            ImGui::TextUnformatted(node.label.c_str());
            for (const auto& pin : node.pins)
            {
                canvas::BeginPin(ids_.pin(pin.id), pin.input ? canvas::PinKind::Input : canvas::PinKind::Output);
                ImGui::Text("%s %s", pin.input ? "<" : ">", pin.label.c_str());
                canvas::EndPin();
            }
            canvas::EndNode();
        }
        for (std::size_t i{}; i < links_.size(); ++i)
            canvas::Link(ids_.link(i + 1), ids_.pin(links_[i].from), ids_.pin(links_[i].to));
        if (canvas::BeginCreate())
        {
            canvas::PinId from, to;
            if (canvas::QueryNewLink(&from, &to) && from && to && canvas::AcceptNewItem())
            {
                CanvasLink link{ids_.source(from), ids_.source(to)};
                bool input{};
                for (const auto& node : nodes_)
                    for (const auto& pin : node.pins)
                        if (pin.id == link.from)
                            input = pin.input;
                if (input)
                    std::swap(link.from, link.to);
                publish({link, true, true, false});
            }
            canvas::EndCreate();
        }
        if (canvas::BeginDelete())
        {
            CanvasErase erase;
            canvas::NodeId node;
            while (canvas::QueryDeletedNode(&node))
                if (canvas::AcceptDeletedItem())
                    erase.nodes.push_back(ids_.source(node));
            canvas::LinkId link;
            while (canvas::QueryDeletedLink(&link))
            {
                const auto source = ids_.source(link);
                if (source && source <= links_.size() && canvas::AcceptDeletedItem())
                    erase.links.push_back(links_[source - 1]);
            }
            if (!erase.nodes.empty() || !erase.links.empty())
                publish({std::move(erase), true, true, false});
            canvas::EndDelete();
        }
        if (canvas::HasSelectionChanged())
        {
            selection_.clear();
            const auto count = canvas::GetSelectedNodes(selected_.data(), static_cast<int>(selected_.size()));
            for (int i{}; i < count; ++i)
                selection_.push_back(ids_.source(selected_[i]));
            delivery_ = emit(selected, std::span<const std::uint64_t>{selection_});
        }
        canvas::End();
        movement_.nodes.clear();
        for (const auto& node : nodes_)
        {
            const auto position = canvas::GetNodePosition(ids_.node(node.id));
            if (position.x != node.position.x || position.y != node.position.y)
                movement_.nodes.push_back({node.id, {position.x, position.y}});
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) && moving_)
            finishEdit(true);
        else if (enabled() && !movement_.nodes.empty() && ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            publish({movement_, !moving_, false, false});
            moving_ = true;
        }
        else if (moving_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
            finishEdit();
    }
}
