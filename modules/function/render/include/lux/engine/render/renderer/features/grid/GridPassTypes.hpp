#pragma once

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
     *   auto* grid = renderer.getFeature<GridPassFeature>();
     *   lux::render::GridParams p;
     *   p.cellSize = 0.5f;   // finer grid
     *   p.fadeDist = 100.0f; // larger fade radius
     *   grid->setGridParams(p);
     * @endcode
     */
    struct GridParams
    {
        float planeY    = 0.0f;   ///< World-space Y of the grid plane
        float cellSize  = 1.0f;   ///< Size of one grid cell in world units
        float linePx    = 1.1f;   ///< Line half-width in screen pixels
        float fadeDist  = 50.0f;  ///< Distance at which the grid fully fades out
        float holeRatio = 0.45f;  ///< Fraction of each cell that is transparent
    };

} // namespace lux::render
