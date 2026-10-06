#include <lux/engine/flowforge/graph/ControlNode.hpp>
#include <lux/engine/flowforge/graph/FlowSource.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <array>
#include <cstdio>

// The same source graph/codec used by the desktop Flow protocol, without its compiler or host.
int main()
{
    namespace flow = lux::flowforge;
    lux::meta::ReflectionRegistry::initRegistry();
    flow::FlowGraph graph;
    const auto index = graph.addNodes(std::make_unique<flow::OnEventNode>("tick"));
    const auto node = graph.getNode(index).node->id();
    if (!graph.addExport({flow::FlowForgeExportNodeId{1}, node, 0x1234}))
        return 1;
    std::array<std::uint8_t, 16> identity{};
    identity.back() = 2;
    const auto source = flow::captureFlowSource(lux::asset::AssetId{identity}, "Flow source", graph);
    if (!source)
        return 1;
    const auto encoded = flow::encodeFlowSource(*source);
    if (!encoded)
        return 1;
    const auto decoded = flow::decodeFlowSource(*encoded);
    if (!decoded || *decoded != *source)
        return 1;
    auto malformed = *decoded;
    malformed.nodes.push_back(malformed.nodes.front());
    if (flow::validateFlowSource(malformed))
        return 1;
    std::puts("PASS native Flow graph/export/source roundtrip and duplicate identity rejection; no linker or UI");
    return 0;
}
