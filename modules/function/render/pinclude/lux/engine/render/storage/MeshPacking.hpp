#pragma once

#include <lux/engine/render/inputs/IndexedTriangleMeshInputs.hpp>
#include <lux/engine/description/Mesh.hpp>

namespace lux::render::detail
{
    // The actual built-in vertex ABI. Packing excludes unused skinning data and preserves tangent handedness.
    struct MeshVertex final
    {
        float position[3];
        float uv[2];
        float normal[3];
        float tangent[4];
    };

    struct PackedMesh final
    {
        MeshDescription description;
        std::vector<MeshVertex> vertices;
    };

    // Worker-only preparation. Index arrays remain borrowed and are copied directly into Staging.
    [[nodiscard]] RenderResult<PackedMesh> packMesh(const rdesc::Mesh& mesh) noexcept;
} // namespace lux::render::detail
