#include <lux/engine/flowforge/Compiler.hpp>

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
    auto artifact = lux::flowforge::compileFlowForgeScript(
        "lux.consumer.flowforge",
        exports,
        lux::flowforge::StateLayout{}
    );
    return artifact && artifact->findExport(1U) ? 0 : 1;
}
