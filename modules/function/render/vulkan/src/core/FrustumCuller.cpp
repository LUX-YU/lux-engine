#include <lux/engine/render/core/FrustumCuller.hpp>
#include <cmath>
#include <algorithm>

namespace lux::render
{

// ─── Extract frustum planes from a row-major float[16] VP matrix ────────
//
// Uses Gribb/Hartmann method: each plane is derived from a combination of
// the VP matrix rows.  The matrix is assumed to be in Eigen's default
// column-major storage, so m[col*4+row].
void extractFrustum(const float* m, Frustum& out)
{
    // Helper: normalise a plane in-place.
    auto normalisePlane = [](Frustum::Plane& p) {
        float len = p.normal.norm();
        if (len > 1e-8f) { p.normal /= len; p.d /= len; }
    };

    // Column-major indexing: element (row, col) = m[col*4 + row]
    auto e = [&](int row, int col) -> float { return m[col * 4 + row]; };

    // Left
    out.planes[Frustum::Left].normal = {e(3,0)+e(0,0), e(3,1)+e(0,1), e(3,2)+e(0,2)};
    out.planes[Frustum::Left].d      =  e(3,3)+e(0,3);
    normalisePlane(out.planes[Frustum::Left]);

    // Right
    out.planes[Frustum::Right].normal = {e(3,0)-e(0,0), e(3,1)-e(0,1), e(3,2)-e(0,2)};
    out.planes[Frustum::Right].d      =  e(3,3)-e(0,3);
    normalisePlane(out.planes[Frustum::Right]);

    // Bottom
    out.planes[Frustum::Bottom].normal = {e(3,0)+e(1,0), e(3,1)+e(1,1), e(3,2)+e(1,2)};
    out.planes[Frustum::Bottom].d      =  e(3,3)+e(1,3);
    normalisePlane(out.planes[Frustum::Bottom]);

    // Top
    out.planes[Frustum::Top].normal = {e(3,0)-e(1,0), e(3,1)-e(1,1), e(3,2)-e(1,2)};
    out.planes[Frustum::Top].d      =  e(3,3)-e(1,3);
    normalisePlane(out.planes[Frustum::Top]);

    // Near (Vulkan ZO depth range [0, 1]): clip_z >= 0  -> row2 * world >= 0
    out.planes[Frustum::Near].normal = {e(2,0), e(2,1), e(2,2)};
    out.planes[Frustum::Near].d      =  e(2,3);
    normalisePlane(out.planes[Frustum::Near]);

    // Far
    out.planes[Frustum::Far].normal = {e(3,0)-e(2,0), e(3,1)-e(2,1), e(3,2)-e(2,2)};
    out.planes[Frustum::Far].d      =  e(3,3)-e(2,3);
    normalisePlane(out.planes[Frustum::Far]);
}

// ─── Frustum static factory ─────────────────────────────────────────────
Frustum Frustum::fromViewProj(const Eigen::Matrix4f& vp)
{
    Frustum f;
    extractFrustum(vp.data(), f);
    return f;
}

// ─── AABB vs Frustum test (conservative — returns true if partially inside) ─
bool Frustum::isAABBInside(const Eigen::Vector3f& aabb_min,
                           const Eigen::Vector3f& aabb_max) const
{
    for (auto& plane : planes)
    {
        // Find the positive vertex (the one most aligned with the plane normal)
        Eigen::Vector3f p_vertex;
        p_vertex.x() = (plane.normal.x() >= 0.f) ? aabb_max.x() : aabb_min.x();
        p_vertex.y() = (plane.normal.y() >= 0.f) ? aabb_max.y() : aabb_min.y();
        p_vertex.z() = (plane.normal.z() >= 0.f) ? aabb_max.z() : aabb_min.z();

        if (plane.normal.dot(p_vertex) + plane.d < 0.f)
            return false; // entirely outside this plane
    }
    return true;
}

// ─── Compute screen-space error ─────────────────────────────────────────
float computeScreenError(const float* aabb_min, const float* aabb_max,
                         const float* /*view_proj*/, const float* camera_pos,
                         float screen_width)
{
    // Centre & diagonal of the AABB
    float cx = (aabb_min[0] + aabb_max[0]) * 0.5f;
    float cy = (aabb_min[1] + aabb_max[1]) * 0.5f;
    float cz = (aabb_min[2] + aabb_max[2]) * 0.5f;

    float dx = aabb_max[0] - aabb_min[0];
    float dy = aabb_max[1] - aabb_min[1];
    float dz = aabb_max[2] - aabb_min[2];
    float radius = std::sqrt(dx*dx + dy*dy + dz*dz) * 0.5f;

    // Distance from the camera to the AABB centre
    float ex = cx - camera_pos[0];
    float ey = cy - camera_pos[1];
    float ez = cz - camera_pos[2];
    float dist = std::sqrt(ex*ex + ey*ey + ez*ez);
    dist = std::max(dist, 0.001f);

    // Approximate screen-space size (pixels) using pinhole model with 90° FoV
    return (radius / dist) * screen_width;
}

// ─── FrustumCuller ──────────────────────────────────────────────────────
bool FrustumCuller::isVisible(const Eigen::Vector3f& aabb_min,
                              const Eigen::Vector3f& aabb_max) const
{
    return frustum_.isAABBInside(aabb_min, aabb_max);
}

} // namespace lux::render
