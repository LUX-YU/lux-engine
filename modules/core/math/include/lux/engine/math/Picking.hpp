#pragma once
#include <lux/engine/math/Ray.hpp>
#include <Eigen/Core>

namespace lux::math
{
    /**
     * @brief Construct a world-space picking ray from screen coordinates.
     *
     * Template to keep the entire definition in the header and avoid
     * MSVC x64 ABI issues at DLL boundaries with Eigen types.
     *
     * @tparam Scalar         Floating-point type (float or double).
     * @param screen_x        Cursor X in window pixels (0 = left edge).
     * @param screen_y        Cursor Y in window pixels (0 = top edge).
     * @param viewport_width  Window / framebuffer width in pixels.
     * @param viewport_height Window / framebuffer height in pixels.
     * @param inv_view_proj   Inverse of the combined View*Proj matrix.
     * @param[out] out_ray    Resulting ray with origin on the near plane
     *                        and normalized direction.
     *
     * Assumes Vulkan-style NDC: x ∈ [-1,1], y ∈ [-1,1], z ∈ [0,1].
     */
    template <typename Scalar = float>
    void screenToRay(
        Scalar screen_x,
        Scalar screen_y,
        Scalar viewport_width,
        Scalar viewport_height,
        const Eigen::Matrix<Scalar, 4, 4>& inv_view_proj,
        Ray& out_ray
    )
    {
        const Scalar ndc_x = (Scalar(2) * screen_x / viewport_width) - Scalar(1);
        const Scalar ndc_y = (Scalar(2) * screen_y / viewport_height) - Scalar(1);

        const Eigen::Matrix<Scalar, 4, 1> near_ndc(ndc_x, ndc_y, Scalar(0), Scalar(1));
        const Eigen::Matrix<Scalar, 4, 1> far_ndc(ndc_x, ndc_y, Scalar(1), Scalar(1));

        const Eigen::Matrix<Scalar, 4, 1> near_world = inv_view_proj * near_ndc;
        const Eigen::Matrix<Scalar, 4, 1> far_world = inv_view_proj * far_ndc;

        out_ray.origin = (near_world.template head<3>() / near_world.w()).template cast<float>();

        // Direction in homogeneous space avoids dividing by the near-zero
        // far_world.w that arises with large far/near ratios.
        const Eigen::Matrix<Scalar, 3, 1> dir =
            far_world.template head<3>() * near_world.w() - near_world.template head<3>() * far_world.w();
        out_ray.direction = dir.normalized().template cast<float>();
    }

} // namespace lux::math
