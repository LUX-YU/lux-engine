#include <lux/engine/flowforge/script/ScriptGraph.hpp>

#include <array>

int main()
{
    const std::array exports{
        lux::flowforge::ExportMethodNode{
            lux::flowforge::FlowForgeExportNodeId{1U},
            1U,
            "tick",
            {},
            {}
        }
    };
    return lux::flowforge::validFlowForgeExports(exports) ? 0 : 1;
}
