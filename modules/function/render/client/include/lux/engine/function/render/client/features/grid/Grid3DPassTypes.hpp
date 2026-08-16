#pragma once

#include <cstdint>

namespace lux::render
{
    /**
     * @brief Configurable appearance parameters for the infinite ground grid.
     *
     * These values are passed as push constants to the grid fragment shader.
     * The defaults reproduce the previous hard-coded behaviour (T3-3).
     *
     * Usage:
     * @code
     *   auto* grid = renderer.getFeature<Grid3DPassFeature>();
     *   lux::render::Grid3DParams p;
     *   p.cellSize = 0.5f;   // finer grid
     *   p.fadeDist = 100.0f; // larger fade radius
     *   grid->setGrid3DParams(p);
     * @endcode
     */
    struct Grid3DParams
    {
        float planeY    = 0.0f;   ///< World-space Y of the grid plane
        float cellSize  = 1.0f;   ///< Size of one grid cell in world units
        float linePx    = 1.1f;   ///< Line half-width in screen pixels
        float fadeDist  = 50.0f;  ///< Distance at which the grid fully fades out
        float holeRatio = 0.45f;  ///< Fraction of each cell that is transparent
        std::uint32_t onTop = 0;  ///< 1 = X-ray (fragment writes near depth), 0 = depth-tested
    };

} // namespace lux::render
