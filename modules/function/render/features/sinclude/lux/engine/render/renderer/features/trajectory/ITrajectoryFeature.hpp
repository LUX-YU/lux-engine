#pragma once
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
    enum class TrajectoryMode : uint8_t
    {
        Line = 1,   ///< LINE_STRIP rendering (simplest, fastest)
        Ribbon = 2, ///< Compute-expanded screen-facing triangle strip
        Tube = 3,   ///< Compute-expanded cylindrical triangle mesh
    };

    /**
     * @brief Extended RenderFeature interface for trajectory rendering modes.
     */
    class LUX_FUNCTION_PUBLIC ITrajectoryFeature : public RenderFeature
    {
    public:
        using RenderFeature::RenderFeature;

        /// The mode this feature implements.
        [[nodiscard]] virtual TrajectoryMode mode() const noexcept = 0;
    };
} // namespace lux::render
