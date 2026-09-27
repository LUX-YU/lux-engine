#pragma once
#include <lux/engine/function/render/features/visibility.h>
/**
 * @file ITrajectoryFeature.hpp
 * @brief Abstract interface for trajectory render features.
 *
 * TrajectoryLineFeature implements this interface; it is the extension seam for
 * any future trajectory rendering mode (e.g. ribbon / tube).
 *
 * Shared GPU resources (TrajectoryGlobalBuffer) are accessed through
 * sceneView().resources().find<TrajectoryResources>().
 */

#include <lux/engine/render/RenderFeature.hpp>
#include <lux/engine/function/visibility.h>

namespace lux::render
{
    /**
     * @brief Identifies the trajectory rendering algorithm.
     *
     * The numerical values are stable — do not reorder.
     */
    enum class ETrajectoryMode : uint8_t
    {
        LINE = 1,   ///< LINE_STRIP rendering (simplest, fastest)
        RIBBON = 2, ///< Compute-expanded screen-facing triangle strip
        TUBE = 3,   ///< Compute-expanded cylindrical triangle mesh
    };

    /**
     * @brief Extended RenderFeature interface for trajectory rendering modes.
     */
    class LUX_ENGINE_FUNCTION_RENDER_FEATURES_PUBLIC ITrajectoryFeature : public RenderFeature
    {
    public:
        using RenderFeature::RenderFeature;

        /// The mode this feature implements.
        [[nodiscard]] virtual ETrajectoryMode mode() const noexcept = 0;
    };
} // namespace lux::render
