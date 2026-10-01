#include <lux/engine/editor/widgets/GraphCanvas.hpp>
#include <lux/engine/ui/Root.hpp>
#include <imgui.h>
#include <algorithm>
#include <nlohmann/json.hpp>
#include <cstring>
namespace lux::editor::widgets
{
    namespace canvas = ax::NodeEditor;
    GraphCanvas::GraphCanvas(lux::ui::Element& parent, lux::ui::ElementId id)
        : Element(parent, std::move(id)), canvas_(makeCanvas())
    {
        setStretch({1, 1});
    }
    NodeCanvas GraphCanvas::makeCanvas()
    {
        canvas::Config config;
        config.SettingsFile = nullptr;
        config.CanvasSizeMode = canvas::CanvasSizeMode::CenterOnly;
        config.UserPointer = this;
        config.SaveSettings = +[](const char* bytes, std::size_t size, canvas::SaveReasonFlags, void* owner) {
            auto& view = *static_cast<GraphCanvas*>(owner);
            const auto settings = nlohmann::json::parse(bytes, bytes + size, nullptr, false);
            if (settings.is_discarded() || !settings.contains("view"))
                return false;
            // No stale node IDs or per-node settings survive backend replacement. Domain layout and
            // selection are restored explicitly; the documented settings channel preserves navigation.
            auto navigation = settings["view"];
            // visible_rect invokes a fit animation on restoration, which rounds the stored camera
            // to the new backend's first rectangle. Preserve the exact scroll/zoom instead.
            navigation.erase("visible_rect");
            view.saved_view_ = nlohmann::json{{"view", std::move(navigation)}}.dump();
            return true;
        };
        config.LoadSettings = +[](char* bytes, void* owner) {
            auto& view = *static_cast<GraphCanvas*>(owner);
            const auto& saved = view.saved_view_;
            // The backend loads settings after its first size-measurement canvas. That probe
            // advances ImGui's cursor; restore our content origin before the actual canvas starts.
            // This callback and the cursor are confined to the current draw (no retained context).
            if (view.drawing_)
                ImGui::SetCursorScreenPos({view.draw_origin_.x, view.draw_origin_.y});
            if (bytes)
                std::memcpy(bytes, saved.data(), saved.size());
            return saved.size();
        };
        return NodeCanvas{canvas::CreateEditor(&config)};
    }
    bool GraphCanvas::setGraph(std::vector<CanvasNode>&& nodes, std::vector<CanvasLink>&& links, bool input_idle)
    {
        if (drawing_ || object::LuxObject::isDispatching())
            return false;
        std::size_t active = nodes.size() + links.size();
        std::size_t missing{};
        for (const auto& node : nodes)
        {
            active += node.pins.size();
            missing += !ids_.hasNode(node.id);
            for (const auto& pin : node.pins)
                missing += !ids_.hasPin(pin.id);
        }
        for (std::size_t i{}; i != links.size(); ++i)
            missing += !ids_.hasLink(i + 1);
        const auto limit = std::max<std::size_t>(4096, 4 * active);
        const bool needs_rebuild = ids_.size() > limit || missing > limit - std::min(ids_.size(), limit);
        if (needs_rebuild)
        {
            // setGraph also runs between Root frames (including explicit rebind). There is no
            // ambient ImGui context there. The owning Root retains the adopted native input facts.
            const auto input = attachedRoot() ? root().inputSnapshot() : lux::ui::InputSnapshot{};
            const bool has_pointer_input = std::ranges::any_of(input.buttons, [](bool down) { return down; });
            const bool is_busy = !input_idle || moving_ || has_pointer_input || input.composing;
            if (is_busy)
                return false;
        }
        if (needs_rebuild)
        {
            // Snapshot the backend's actual selection at this safe point. A selection notification
            // cache need not include changes made through the backend between frames.
            {
                CanvasScope scope(canvas_.get());
                const auto count = canvas::GetSelectedNodes(selected_.data(), static_cast<int>(selected_.size()));
                selection_.clear();
                for (int i{}; i < count; ++i)
                    selection_.push_back(ids_.source(selected_[i]));
            }
            // No CanvasScope may retain the old context while it is destroyed.
            canvas_.reset();
            ids_.retire();
            canvas_ = makeCanvas();
        }
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
        if (needs_rebuild)
        {
            std::erase_if(selection_, [&](auto id) {
                return std::ranges::find(nodes_, id, &CanvasNode::id) == nodes_.end();
            });
            for (auto id : selection_)
                canvas::SelectNode(ids_.node(id), true);
        }
        return true;
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
        struct Drawing final
        {
            bool& flag;
            explicit Drawing(bool& value) : flag(value)
            {
                flag = true;
            }
            ~Drawing()
            {
                flag = false;
            }
        } drawing(drawing_);
        CanvasScope scope(canvas_.get());
        const auto origin = ImGui::GetCursorScreenPos();
        draw_origin_ = {origin.x, origin.y};
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
        // Node-editor applies selection input in End, so observe the resulting selection afterward.
        canvas::End();
        if (canvas::HasSelectionChanged())
        {
            selection_.clear();
            const auto count = canvas::GetSelectedNodes(selected_.data(), static_cast<int>(selected_.size()));
            for (int i{}; i < count; ++i)
                selection_.push_back(ids_.source(selected_[i]));
            delivery_ = emit(selected, std::span<const std::uint64_t>{selection_});
        }
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
