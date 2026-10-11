#include <lux/engine/render/vulkan/graph/Executable.hpp>
#include <lux/engine/render/vulkan/shader/Bindings.hpp>

// Force the real binding recipe object file into this consumer without any cold compiler call.
// Qualification inspects the resulting PE imports/link map, not just header spelling.
auto volatile binding_function = &lux::render::vulkan::BoundDescriptorSets::bind;
auto volatile graph_function = &lux::render::vulkan::ExecutableGraphPlan::submit;

int main()
{
    return binding_function == nullptr || graph_function == nullptr;
}
