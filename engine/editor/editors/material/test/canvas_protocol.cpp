#include <cassert>
#include <cmath>
#include <cstdio>
#include <imgui.h>
#include <limits>
#include <lux/engine/editor/gui/NodeCanvasIds.hpp>

int main()
{
    lux::editor::gui::NodeCanvasIds ids;
    const auto node = ids.node(1);
    const auto pin = ids.pin(1);
    const auto link = ids.link(1);
    assert(node.Get() != pin.Get() && pin.Get() != link.Get() && node.Get() != link.Get());
    assert(ids.source(node) == 1 && ids.source(pin) == 1 && ids.source(link) == 1);
    assert(ids.source(ax::NodeEditor::NodeId{pin.Get()}) == 0);
    assert(ids.source(ax::NodeEditor::PinId{0}) == 0);
    assert(ids.source(ax::NodeEditor::LinkId{999}) == 0);

    const auto large = ids.node(std::numeric_limits<std::uint64_t>::max());
    for (std::uint64_t id = 2; id < 4096; ++id)
    {
        static_cast<void>(ids.node(id));
        static_cast<void>(ids.pin(id));
    }
    assert(ids.node(1) == node && ids.pin(1) == pin);
    assert(ids.source(large) == std::numeric_limits<std::uint64_t>::max());

    IMGUI_CHECKVERSION();
    auto *context = ImGui::CreateContext();
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {1000, 700};
    io.DeltaTime = 1.0F / 60;
    unsigned char *pixels{};
    int width{}, height{};
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    ax::NodeEditor::Config config;
    config.SettingsFile = nullptr;
    config.CanvasSizeMode = ax::NodeEditor::CanvasSizeMode::CenterOnly;
    auto *graph = ax::NodeEditor::CreateEditor(&config);
    auto *other_graph = ax::NodeEditor::CreateEditor(&config);
    lux::editor::gui::NodeCanvasIds other_ids;
    const auto other_node = other_ids.node(1);
    const auto other_pin = other_ids.pin(1);
    ax::NodeEditor::SetCurrentEditor(graph);
    ax::NodeEditor::SetNodePosition(node, {40, 60});
    ax::NodeEditor::SetCurrentEditor(other_graph);
    ax::NodeEditor::SetNodePosition(other_node, {180, 120});

    for (int frame = 0; frame < 3; ++frame)
    {
        ImGui::NewFrame();
        ax::NodeEditor::SetCurrentEditor(graph);
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(frame == 0 ? ImVec2{440, 80} : ImVec2{900, 600});
        ImGui::Begin("Canvas protocol");
        ax::NodeEditor::Begin("Graph");
        ax::NodeEditor::BeginNode(node);
        ImGui::TextUnformatted("Node 1");
        ax::NodeEditor::BeginPin(pin, ax::NodeEditor::PinKind::Output);
        ImGui::TextUnformatted("Pin 1");
        ax::NodeEditor::EndPin();
        ax::NodeEditor::EndNode();
        const auto screen = ax::NodeEditor::CanvasToScreen({40, 60});
        const auto roundtrip = ax::NodeEditor::ScreenToCanvas(screen);
        assert(std::abs(roundtrip.x - 40) < 0.01F && std::abs(roundtrip.y - 60) < 0.01F);
        ax::NodeEditor::End();
        ImGui::End();
        assert(ax::NodeEditor::GetCurrentZoom() == 1.0F);
        assert(ax::NodeEditor::GetNodeSize(node).x > 0);
        ax::NodeEditor::SelectNode(node);

        ax::NodeEditor::SetCurrentEditor(other_graph);
        ImGui::SetNextWindowPos({450, 80});
        ImGui::SetNextWindowSize({400, 300});
        ImGui::Begin("Second document");
        ax::NodeEditor::Begin("Graph");
        ax::NodeEditor::BeginNode(other_node);
        ImGui::TextUnformatted("Node 1");
        ax::NodeEditor::BeginPin(other_pin, ax::NodeEditor::PinKind::Output);
        ImGui::TextUnformatted("Pin 1");
        ax::NodeEditor::EndPin();
        ax::NodeEditor::EndNode();
        ax::NodeEditor::End();
        ImGui::End();
        assert(!ax::NodeEditor::IsNodeSelected(other_node));
        const auto other_position = ax::NodeEditor::GetNodePosition(other_node);
        assert(other_position.x == 180 && other_position.y == 120);
        ImGui::Render();

        if (frame == 1)
        {
            // Recreate only the first pane, restoring its persisted business position.
            ax::NodeEditor::DestroyEditor(graph);
            graph = ax::NodeEditor::CreateEditor(&config);
            ax::NodeEditor::SetCurrentEditor(graph);
            ax::NodeEditor::SetNodePosition(node, {40, 60});
            assert(!ax::NodeEditor::IsNodeSelected(node));
        }
    }

    ax::NodeEditor::DestroyEditor(graph);
    ax::NodeEditor::DestroyEditor(other_graph);
    ImGui::DestroyContext(context);
    std::puts("PASS node canvas: disjoint IDs, full-width source identity, resize preserves zoom; two contexts with "
              "same local IDs keep positions/selection isolated; pane recreation and coordinate roundtrip");
}
