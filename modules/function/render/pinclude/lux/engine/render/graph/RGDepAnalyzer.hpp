#pragma once
#include <lux/engine/render/graph/RGPassTypes.hpp>

namespace lux::render
{
    struct RGTransientLifetime
    {
        RGResourceHandle resource;
        uint32_t         first_pass = 0; // Index in passes array
        uint32_t         last_pass = 0;
    };

    // If you want to store barrier info later, you can add it here
    struct RGDependencyInfo
    {
        // Topological sort result of passes: index of RGGraphDescription::passes
        std::vector<uint32_t>            pass_topological_order;
        // Lifetime of each resource within this frame
        std::vector<RGTransientLifetime> lifetime;
        // True if the dependency graph contains a cycle
        bool                             has_cycle = false;
    };

    // Class responsible only for dependency analysis (pure logic, no Vulkan involved)
    class LUX_FUNCTION_PUBLIC DependencyAnalyzer
    {
    public:
        // Input: High-level Graph description
        // Output: Topological order + resource lifetime
        [[nodiscard]] static RGDependencyInfo analyze(const RGGraphDescription& graph);
    };
}
