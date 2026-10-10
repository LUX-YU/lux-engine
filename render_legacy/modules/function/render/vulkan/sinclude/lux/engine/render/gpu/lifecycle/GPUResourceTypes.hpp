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
        INSTANCE = 0,  // Instance resources
        LIGHT,         // Light resources
        MATERIAL,      // Material resources
        MESH,          // Mesh resources
        SCENE,         // Scene resources
        TEXTURE,       // Texture resources
        POINT_CLOUD,   // Point cloud resources
        PARTICLE,      // GPU particle SSBO
        COMPUTE,       // GPU-driven compute cull
        SHADOW,        // Shadow atlas + SSBO/UBO
        SHADER,        // Compiled shader modules
        TRAJECTORY,    // Trajectory line/ribbon/tube rendering
        VERTEX_POOL,   // Bindless vertex source array (R1.4 of render-refactor)
    };

    /// Number of enumerators in EGPUResourceType (keep in sync with enum above).
    inline constexpr size_t kGPUResourceTypeCount = 13;

} // namespace lux::render
