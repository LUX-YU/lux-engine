#pragma once
#include <lux/engine/description/Vertex.hpp>
#include <lux/engine/math/AABB.hpp>
#include <vector>
#include <cstdint>
#include <optional>
#include <limits>

namespace lux::rdesc
{
    /**
     * @brief One discrete level of detail: a simplified index list over the
     *        mesh's ORIGINAL vertices, so every LOD shares LOD0's vertex buffer.
     *        Auto-generated at import time via meshopt_simplify.
     */
    struct MeshLod
    {
        std::vector<uint32_t> indices;     ///< Simplified triangle indices into Mesh::vertices
        float                 error{0.0f}; ///< meshopt relative-error estimate for this level
    };

    /**
     * @brief Structure representing a complete mesh with vertices and indices.
     *
     * A mesh contains vertex data and index data that define the geometric
     * structure of a 3D object.
     */
    struct Mesh
    {
        std::vector<Vertex>     vertices; ///< Array of vertices (shared by LOD0 + all `lods`)
        std::vector<uint32_t>   indices;  ///< LOD0 triangle indices
        std::optional<lux::math::AABB> bounds; ///< Pre-computed AABB (set during asset loading)
        std::vector<MeshLod>    lods;     ///< LOD1..N (empty == single-LOD; static meshes only)
    };

    [[nodiscard]] inline std::optional<std::size_t> meshRetainedBytes(
        const Mesh& mesh
    ) noexcept
    {
        std::size_t bytes = sizeof(Mesh);
        const auto add = [&bytes](std::size_t count, std::size_t stride)
        {
            if (stride != 0 &&
                count > (std::numeric_limits<std::size_t>::max() - bytes) /
                            stride)
                return false;
            bytes += count * stride;
            return true;
        };

        if (!add(mesh.vertices.capacity(), sizeof(Vertex)) ||
            !add(mesh.indices.capacity(), sizeof(std::uint32_t)) ||
            !add(mesh.lods.capacity(), sizeof(MeshLod)))
            return std::nullopt;
        for (const auto& lod : mesh.lods)
            if (!add(lod.indices.capacity(), sizeof(std::uint32_t)))
                return std::nullopt;
        return bytes;
    }
}
