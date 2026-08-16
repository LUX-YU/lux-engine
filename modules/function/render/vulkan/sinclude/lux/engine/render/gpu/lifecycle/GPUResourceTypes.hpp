#pragma once
#include <cstdint>
#include <cstddef>

namespace lux::render
{
    /**
     * @brief GPU resource type enumeration
     * @details Defines all manageable GPU resource types.
     *          Located in core/ to avoid circular dependencies between
     *          pipeline/ and resources/ layers.
     */
    enum class EGPUResourceType : uint8_t
    {
        Instance = 0,   // Instance resources
        Light,          // Light resources
        Material,       // Material resources
        Mesh,           // Mesh resources
        Scene,          // Scene resources
        Texture,        // Texture resources
        PointCloud,     // Point cloud resources
        Particle,       // GPU particle SSBO
        Compute,        // GPU-driven compute cull
        Shadow,         // Shadow atlas + SSBO/UBO
        Shader,         // Compiled shader modules
        Trajectory,     // Trajectory line/ribbon/tube rendering
        VertexPool,     // Bindless vertex source array (R1.4 of render-refactor)
    };

    /// Number of enumerators in EGPUResourceType (keep in sync with enum above).
    inline constexpr size_t kGPUResourceTypeCount = 13;

} // namespace lux::render
