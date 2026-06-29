#pragma once
/**
 * @file IPointCloudFeature.hpp
 * @brief Abstract interface for multi-mode point cloud render features.
 *
 * All new-generation point cloud features (PCFeatureSimple, PCFeatureGPUDriven,
 * PCFeatureLOD, PCFeatureSplatting) implement this interface.
 *
 * Shared GPU resources (PointCloudGlobalBuffer, GpuOctreeNodeBuffer) are
 * accessed through GPUResourceRegistry::getResource<PointCloudResources>()
 * during construction.  There is no bindResources() call.
 *
 * @see PCFeatureSimple
 * @see PCFeatureIndirectBase
 * @see PointCloudResources
 */

#include <lux/engine/render/RenderFeature.hpp>
#include <lux/engine/function/visibility.h>

namespace lux::render
{
    // =========================================================================
    //  Rendering mode enumeration
    // =========================================================================

    /**
     * @brief Identifies the point cloud rendering algorithm.
     *
     * The numerical values are stable — do not reorder (stored in save files /
     * config).
     */
    enum class EPointCloudMode : uint8_t
    {
        SIMPLE      = 1,  ///< Global SSBO, multi-draw (default)
        GPU_DRIVEN  = 2,  ///< Compute-shader frustum cull + single indirect draw
        LOD         = 3,  ///< Depth-scaled point size
        SPLATTING   = 4,  ///< Gaussian soft-splat rasterization
        TRANSIENT   = 5,  ///< Current-frame-only, zero-accumulation rendering
    };

    // =========================================================================
    //  IPointCloudFeature
    // =========================================================================

    /**
     * @brief Extended RenderFeature interface for new-generation point cloud modes.
     *
     * Concrete implementations operate via the comm protocol.
     * GPU resources are fetched from GPUResourceRegistry during construction.
     */
    class LUX_FUNCTION_PUBLIC IPointCloudFeature : public RenderFeature
    {
    public:
        using RenderFeature::RenderFeature;  // forward config ctor

        /// The mode this feature implements.
        [[nodiscard]] virtual EPointCloudMode mode() const noexcept = 0;

        /// Adjust the screen-pixel point size at runtime.
        ///
        /// Safe to call from any thread; implementations must use atomic stores
        /// so the kernel lambda (render thread) sees a consistent value.
        ///
        /// Per-mode semantics:
        ///   - SIMPLE / GPU_DRIVEN / TRANSIENT : @p pixels becomes the literal
        ///                                      screen-pixel point size.
        ///   - LOD / SPLATTING                : @p pixels becomes the upper
        ///                                      clamp (lod_pc.max_size); the
        ///                                      world radius and min clamp are
        ///                                      preserved from initial Config.
        virtual void setPointSize(float pixels) noexcept = 0;
    };

} // namespace lux::render
