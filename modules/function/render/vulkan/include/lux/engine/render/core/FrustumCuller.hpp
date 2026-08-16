#pragma once
#include <lux/engine/function/visibility.h>
#include <Eigen/Core>
#include <array>
#include <cstddef>
#include <cstdint>

namespace lux::render
{
    /// @brief Six frustum planes (normal + distance) extracted from view-projection matrix.
    struct LUX_FUNCTION_PUBLIC Frustum
    {
        /// Plane equation: normal.dot(point) + d = 0.
        struct Plane
        {
            Eigen::Vector3f normal{0.f, 0.f, 0.f};
            float           d{0.f};
        };

        enum Side : uint32_t { Left = 0, Right, Bottom, Top, Near, Far, Count };

        std::array<Plane, Count> planes{};

        /// Extract frustum planes from a row-major view-projection matrix.
        static Frustum fromViewProj(const Eigen::Matrix4f& vp);

        /// Test AABB (min, max vectors) against frustum planes.
        [[nodiscard]] bool isAABBInside(
            const Eigen::Vector3f& aabb_min,
            const Eigen::Vector3f& aabb_max
        ) const;
    };

    /// GPU view-cull input. Frustum planes are expressed relative to the exact
    /// view origin carried alongside them. This avoids ever collapsing a large
    /// world coordinate into one float before culling.
    struct alignas(16) ViewCullData
    {
        Frustum frustum;
        std::int32_t origin_page[4]{};
        float origin_local_page_size[4]{0.0f, 0.0f, 0.0f, 1024.0f};
    };
    static_assert(sizeof(Frustum) == 96);
    static_assert(sizeof(ViewCullData) == 128);

    inline constexpr std::size_t kViewFrustumStrideBytes = sizeof(ViewCullData);

    /// Extract frustum from a raw float[16] view-projection matrix (row-major).
    LUX_FUNCTION_PUBLIC void extractFrustum(const float* view_proj, Frustum& out);

    /// Extract frustum from an Eigen::Matrix4f view-projection matrix.
    inline void extractFrustum(const Eigen::Matrix4f& vp, Frustum& out)
    {
        extractFrustum(vp.data(), out);
    }

    /// Compute approximate screen-space error for an AABB node.
    float computeScreenError(const float* aabb_min, const float* aabb_max,
                             const float* view_proj, const float* camera_pos,
                             float screen_width);

    /// @brief Frustum-based visibility culler.
    class FrustumCuller
    {
    public:
        explicit FrustumCuller(const Frustum& f) : frustum_(f) {}

        [[nodiscard]] const Frustum& frustum() const noexcept { return frustum_; }

        /// Returns true if an AABB (min, max) is at least partially inside the frustum.
        [[nodiscard]] bool isVisible(
            const Eigen::Vector3f& aabb_min,
            const Eigen::Vector3f& aabb_max
        ) const;

    private:
        Frustum frustum_;
    };

} // namespace lux::render
