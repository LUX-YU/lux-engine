#include <lux/engine/flowforge/compiler/ScriptArtifactCompiler.hpp>

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
        lux::flowforge::FlowForgeStateLayout{0U, 0U, 1U, {}}
    );
    return artifact ? 0 : 1;
}
