#pragma once
/**
 * @file BuiltinGeometry.hpp
 * @brief Procedural builders for the engine's built-in primitive meshes.
 *
 * Three helpers — `buildCube`, `buildFloorPlane` and `buildSphere` — produce
 * vertex / index arrays for the primitive meshes that the editor needs
 * out of the box (the sphere is the material-preview "ball"). They are
 * shared between:
 *
 *   1. `lux_content_baker` (build-time tool) — instantiates these meshes,
 *      wraps them into `MeshAsset` objects, and writes the corresponding
 *      `.luxasset` files into `engine_content/Meshes/`. Those files are
 *      the canonical engine content shipped alongside the editor.
 *
 *   2. `EditorBuiltins` (runtime fallback) — when the on-disk engine
 *      content is missing (e.g. the install directory got corrupted, or
 *      a developer hand-deleted the baked files between runs), the editor
 *      falls back to generating the same meshes in memory. The fallback
 *      lets the editor still boot, with a stderr warning, instead of
 *      crashing.
 *
 * Geometry conventions:
 *   - `buildCube(verts, indices, half)` produces a cube spanning
 *     `[-half, half]³` with per-face normals, tangents and a 0..1 UV
 *     per face.
 *   - `buildFloorPlane(verts, indices, half_extent)` produces a quad
 *     spanning `[-half_extent, half_extent]²` in the XZ plane at `y=0`,
 *     normal = +Y, with UV scaled so the same texture tiles `half_extent/2`
 *     times across the face.
 *   - `buildSphere(verts, indices, radius, segments, rings)` produces a
 *     UV sphere centred at the origin, spanning `[-radius, radius]³`, with
 *     outward normals, longitude-aligned tangents and an equirectangular
 *     `(u = longitude, v = latitude)` UV. Front faces are CCW (outward).
 */

#include <lux/engine/description/Vertex.hpp>

#include <Eigen/Core>
#include <Eigen/Geometry>   // Vector3f::cross (sphere bitangent)

#include <cmath>
#include <cstdint>
#include <vector>

namespace lux::toolchain
{
    inline void buildCubeGeometry(std::vector<lux::rdesc::Vertex>& verts,
                                  std::vector<std::uint32_t>&      indices,
                                  float                            half)
    {
        using V3 = Eigen::Vector3f;
        using V2 = Eigen::Vector2f;

        struct FaceInfo { V3 n, t, bt; V3 corners[4]; };
        const FaceInfo faces[6] = {
            {{ 0, 0, 1},{ 1, 0, 0},{0, 1, 0}, {{-half,-half, half},{ half,-half, half},{ half, half, half},{-half, half, half}}},
            {{ 0, 0,-1},{-1, 0, 0},{0, 1, 0}, {{ half,-half,-half},{-half,-half,-half},{-half, half,-half},{ half, half,-half}}},
            {{ 1, 0, 0},{ 0, 0,-1},{0, 1, 0}, {{ half,-half, half},{ half,-half,-half},{ half, half,-half},{ half, half, half}}},
            {{-1, 0, 0},{ 0, 0, 1},{0, 1, 0}, {{-half,-half,-half},{-half,-half, half},{-half, half, half},{-half, half,-half}}},
            {{ 0, 1, 0},{ 1, 0, 0},{0, 0, 1}, {{-half, half, half},{ half, half, half},{ half, half,-half},{-half, half,-half}}},
            {{ 0,-1, 0},{ 1, 0, 0},{0, 0,-1}, {{-half,-half,-half},{ half,-half,-half},{ half,-half, half},{-half,-half, half}}},
        };
        const V2 uvs[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};

        for (const auto& f : faces)
        {
            const auto base = static_cast<std::uint32_t>(verts.size());
            for (int i = 0; i < 4; ++i)
                verts.push_back({f.corners[i], f.n, f.t, uvs[i], f.bt});
            indices.insert(indices.end(),
                {base, base + 1, base + 2, base, base + 2, base + 3});
        }
    }

    inline void buildFloorPlaneGeometry(std::vector<lux::rdesc::Vertex>& verts,
                                        std::vector<std::uint32_t>&      indices,
                                        float                            half_extent)
    {
        using V3 = Eigen::Vector3f;
        using V2 = Eigen::Vector2f;

        const V3 n(0, 1, 0), t(1, 0, 0), bt(0, 0, 1);
        const float uv_scale = half_extent / 2.0f;
        const V3 corners[4] = {
            {-half_extent, 0, -half_extent}, { half_extent, 0, -half_extent},
            { half_extent, 0,  half_extent}, {-half_extent, 0,  half_extent}
        };
        const V2 uvs[4] = {{0, 0}, {uv_scale, 0}, {uv_scale, uv_scale}, {0, uv_scale}};

        const auto base = static_cast<std::uint32_t>(verts.size());
        for (int i = 0; i < 4; ++i)
            verts.push_back({corners[i], n, t, uvs[i], bt});
        indices.insert(indices.end(),
            {base, base + 2, base + 1, base, base + 3, base + 2});
    }

    /// UV sphere centred at the origin. @p segments = longitude subdivisions,
    /// @p rings = latitude subdivisions. Normals point outward; tangents run
    /// along +longitude; UV is equirectangular. (Pole rows produce a few
    /// zero-area triangles — harmless, and far simpler than a pole fan.)
    inline void buildSphereGeometry(std::vector<lux::rdesc::Vertex>& verts,
                                    std::vector<std::uint32_t>&      indices,
                                    float                            radius,
                                    std::uint32_t                    segments = 32,
                                    std::uint32_t                    rings    = 16)
    {
        using V3 = Eigen::Vector3f;
        using V2 = Eigen::Vector2f;
        constexpr float kPi = 3.14159265358979323846f;

        if (segments < 3) segments = 3;
        if (rings    < 2) rings    = 2;

        const auto base   = static_cast<std::uint32_t>(verts.size());
        const auto stride = segments + 1;

        // (rings+1) × (segments+1) grid; the seam column is duplicated so the
        // wrap-around UV (u: 0..1) and tangent stay continuous.
        for (std::uint32_t y = 0; y <= rings; ++y)
        {
            const float v       = static_cast<float>(y) / static_cast<float>(rings);
            const float phi     = v * kPi;            // 0 (north) .. pi (south)
            const float sin_phi = std::sin(phi);
            const float cos_phi = std::cos(phi);

            for (std::uint32_t x = 0; x <= segments; ++x)
            {
                const float u      = static_cast<float>(x) / static_cast<float>(segments);
                const float theta  = u * 2.0f * kPi;  // 0 .. 2pi (longitude)
                const float sin_th = std::sin(theta);
                const float cos_th = std::cos(theta);

                const V3 n(sin_phi * cos_th, cos_phi, sin_phi * sin_th); // unit, outward
                const V3 pos = n * radius;
                const V3 t(-sin_th, 0.0f, cos_th);                       // d(pos)/d(theta)
                const V3 bt = n.cross(t).normalized();                   // matches cube TBN

                verts.push_back({pos, n, t, V2(u, v), bt});
            }
        }

        // Two CCW-outward triangles per grid cell.
        for (std::uint32_t y = 0; y < rings; ++y)
        {
            for (std::uint32_t x = 0; x < segments; ++x)
            {
                const std::uint32_t i0 = base + y * stride + x;        // top-left
                const std::uint32_t i1 = i0 + 1;                       // top-right
                const std::uint32_t i2 = i0 + stride;                  // bottom-left
                const std::uint32_t i3 = i2 + 1;                       // bottom-right
                indices.insert(indices.end(), {i0, i1, i2, i2, i1, i3});
            }
        }
    }

} // namespace lux::toolchain
