#include <Eigen/Geometry>
#include <algorithm>
#include <cstring>
#include <lux/engine/render/runtime/commands/Error.hpp>
#include <lux/engine/render/resource/mesh/Binding.hpp>
#include <lux/engine/render/resource/ResourceDefinition.hpp>
#include <lux/engine/render/resource/mesh/MeshPacking.hpp>

namespace lux::render::detail
{
    RenderResult<PackedMesh> packMesh(const rdesc::Mesh& mesh) noexcept
    {
        auto description = describeIndexedMesh(mesh);
        if (!description)
        {
            return cxx::unexpected(description.error());
        }
        const auto valid_indices = [&mesh](std::span<const std::uint32_t> indices) noexcept
        {
            return std::ranges::all_of(
                indices,
                [&mesh](std::uint32_t index) noexcept { return index < mesh.vertices.size(); }
            );
        };
        if (!valid_indices(mesh.indices))
        {
            return cxx::unexpected(RenderError{kStorageContract});
        }
        for (const auto& lod : mesh.lods)
        {
            if (!valid_indices(lod.indices))
            {
                return cxx::unexpected(RenderError{kStorageContract});
            }
        }
        PackedMesh result{std::move(*description), {}};
        result.vertices.resize(mesh.vertices.size());
        for (std::size_t i = 0; i < mesh.vertices.size(); ++i)
        {
            const auto& input = mesh.vertices[i];
            auto& output = result.vertices[i];
            std::memcpy(output.position, input.position.data(), sizeof(output.position));
            std::memcpy(output.uv, input.uv.data(), sizeof(output.uv));
            std::memcpy(output.normal, input.normal.data(), sizeof(output.normal));
            std::memcpy(output.tangent, input.tangent.data(), 3 * sizeof(float));
            output.tangent[3] = input.normal.cross(input.tangent).dot(input.bitangent) < 0 ? -1.f : 1.f;
        }
        return result;
    }
} // namespace lux::render::detail
