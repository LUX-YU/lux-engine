#include <lux/engine/render/storage/MeshPacking.hpp>
#include <cmath>
#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <lux/engine/render/extensions/storage/IndexedTriangleMesh.hpp>

namespace lux::render
{
    namespace
    {
        RenderResult<std::size_t> retained(const MeshDescription& description, std::size_t bytes) noexcept
        {
            const auto attributes = cxx::checkedMul(description.attributes.capacity(), sizeof(MeshAttribute));
            const auto submeshes = cxx::checkedMul(description.submeshes.capacity(), sizeof(MeshSubmesh));
            if (!attributes || !submeshes)
            {
                return cxx::unexpected(RenderError{kCommandContract});
            }
            const auto first = cxx::checkedAdd(bytes, *attributes);
            if (!first)
            {
                return cxx::unexpected(RenderError{kCommandContract});
            }
            const auto total = cxx::checkedAdd(*first, *submeshes);
            if (!total)
            {
                return cxx::unexpected(RenderError{kCommandContract});
            }
            return *total;
        }
    } // namespace

    RenderResult<MeshDescription> describeIndexedMesh(const rdesc::Mesh& mesh) noexcept
    {
        const bool invalid_counts = mesh.vertices.empty() || mesh.vertices.size() > UINT32_MAX ||
            mesh.indices.empty() || mesh.indices.size() > UINT32_MAX || mesh.indices.size() % 3 != 0;
        if (invalid_counts)
        {
            return cxx::unexpected(RenderError{kCommandContract});
        }
        MeshDescription result;
        result.vertex_stride = sizeof(detail::MeshVertex);
        result.vertex_count = static_cast<std::uint32_t>(mesh.vertices.size());
        result.index_offset = mesh.vertices.size() * std::uint64_t(sizeof(detail::MeshVertex));
        result.index_count = static_cast<std::uint32_t>(mesh.indices.size());
        result.attributes = {
            {0, offsetof(detail::MeshVertex, position), rdesc::ETextureFormat::RGB32_SFLOAT},
            {1, offsetof(detail::MeshVertex, uv), rdesc::ETextureFormat::RG32_SFLOAT},
            {2, offsetof(detail::MeshVertex, normal), rdesc::ETextureFormat::RGB32_SFLOAT},
            {3, offsetof(detail::MeshVertex, tangent), rdesc::ETextureFormat::RGBA32_SFLOAT}
        };
        result.submeshes.push_back({0, result.index_count, 0});
        result.lods.reserve(mesh.lods.size());
        for (const auto& lod : mesh.lods)
        {
            const bool invalid_lod = lod.indices.empty() || lod.indices.size() % 3 != 0 ||
                lod.indices.size() > UINT32_MAX - result.index_count || !std::isfinite(lod.error) || lod.error < 0;
            if (invalid_lod)
            {
                return cxx::unexpected(RenderError{kCommandContract});
            }
            const auto count = static_cast<std::uint32_t>(lod.indices.size());
            result.lods.push_back({result.index_count, count, lod.error});
            result.index_count += count;
        }
        if (mesh.bounds)
        {
            const auto center = mesh.bounds->center();
            const auto extent = mesh.bounds->halfExtents();
            result.bounds = {{center.x(), center.y(), center.z(), 1}, {extent.x(), extent.y(), extent.z(), 0}};
        }
        return result;
    }

    RenderResult<std::size_t> ConfigValueTraits<MeshDescription>::retainedBytes(const MeshDescription& input) noexcept
    {
        auto bytes = retained(input, sizeof(input));
        const auto lod_bytes = cxx::checkedMul(input.lods.capacity(), sizeof(MeshLodRange));
        if (!bytes || !lod_bytes)
        {
            return cxx::unexpected(RenderError{kCommandContract});
        }
        const auto total = cxx::checkedAdd(*bytes, *lod_bytes);
        if (!total)
        {
            return cxx::unexpected(RenderError{kCommandContract});
        }
        return *total;
    }

    RenderResult<std::size_t> ConfigValueTraits<MeshBinding>::retainedBytes(const MeshBinding& input) noexcept
    {
        return sizeof(input) + input.attributes.capacity() * sizeof(MeshAttribute);
    }

} // namespace lux::render
