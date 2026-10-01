#pragma once
#include <lux/engine/flowforge/graph/FlowSource.hpp>

namespace lux::editor::flowforge
{
    // The only writable authoring representation. FlowSource is its owning, pointer-free capture.
    struct FlowAuthoringSource final
    {
        lux::asset::AssetId id;
        std::string name;
        lux::flowforge::FlowGraph graph;
    };
}
