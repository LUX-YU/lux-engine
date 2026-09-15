#include <cassert>
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
    ax::NodeEditor::SetCurrentEditor(graph);

    for (int frame = 0; frame < 3; ++frame)
    {
        ImGui::NewFrame();
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
        ax::NodeEditor::End();
        ImGui::End();
        ImGui::Render();
        assert(ax::NodeEditor::GetCurrentZoom() == 1.0F);
        assert(ax::NodeEditor::GetNodeSize(node).x > 0);
    }

    ax::NodeEditor::DestroyEditor(graph);
    ImGui::DestroyContext(context);
    std::puts("PASS node canvas: disjoint IDs, full-width source identity, stable restoration, resize preserves zoom");
}
