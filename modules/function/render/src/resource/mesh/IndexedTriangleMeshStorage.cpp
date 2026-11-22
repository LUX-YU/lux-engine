#include <lux/engine/render/resource/mesh/MeshLayout.hpp>
#include <lux/engine/render/resource/RasterProgramBuild.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/device/BufferArena.hpp>
#include <lux/engine/render/device/NativeResult.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/graph/Shader.hpp>
#include <lux/engine/render/resource/mesh/IndexedTriangleMeshStorage.hpp>
#include <lux/engine/render/resource/mesh/MeshPacking.hpp>

namespace lux::render::detail
{
    namespace
    {
        exec::task<MeshCommands::UploadMesh::Response>
        createMesh(IndexedTriangleMeshStorage& resource, MeshCommands::UploadMesh::Request&& input) noexcept
        {
            if (!input.mesh)
            {
                co_return MeshCommands::UploadMesh::Response{RenderError{kCommandContract}};
            }
            auto uploaded = co_await resource.upload(*input.mesh);
            co_return uploaded ? MeshCommands::UploadMesh::Response{{}, *uploaded}
                               : MeshCommands::UploadMesh::Response{uploaded.error()};
        }

        void
        releaseMesh(
            IndexedTriangleMeshStorage& resource,
            const MeshCommands::ReleaseMesh::Request& input,
            MeshCommands::ReleaseMesh::Response& response
        ) noexcept
        {
            const auto released = resource.release(input.data);
            response.error = released ? RenderError{} : released.error();
        }
    } // namespace

    IndexedTriangleMeshStorage::IndexedTriangleMeshStorage(
        ResourceCreateContext& dependencies,
        ResourceInstanceId id
    ) noexcept
        : session_(dependencies.device), uploads_(*dependencies.uploads), id_(id),
          vertices_(dependencies.device, nri::BufferUsageBits::VERTEX),
          indices_(dependencies.device, nri::BufferUsageBits::INDEX),
          sections_(
              dependencies.device,
              (nri::BufferUsageBits::SHADER_RESOURCE | nri::BufferUsageBits::SHADER_RESOURCE_STORAGE),
              65536
          )
    {
    }

    IndexedTriangleMeshStorage::~IndexedTriangleMeshStorage() noexcept
    {
        for (auto& entry : entries_)
        {
            retire(entry.geometry);
        }
    }

    std::size_t IndexedTriangleMeshStorage::recordCount() const noexcept
    {
        return entries_.size();
    }

    RenderResult<void> IndexedTriangleMeshStorage::validate(const MeshDescription& description) const noexcept
    {
        const auto& limits = session_.description();
        const auto& bounds = description.bounds;
        const bool invalid_bounds =
            bounds.center[3] != 0 &&
            (bounds.center[3] != 1 || !std::ranges::all_of(bounds.center, [](float v) { return std::isfinite(v); }) ||
             !std::ranges::all_of(bounds.half_extent, [](float v) { return std::isfinite(v) && v >= 0; }));
        if (invalid_bounds)
        {
            return cxx::unexpected(RenderError{kStorageContract});
        }
        const bool invalid_vertices = !description.vertex_count || !description.vertex_stride ||
                                      description.vertex_stride > session_.vertexStrideLimit() ||
                                      description.vertex_stride % 4;
        const bool invalid_indices =
            !description.index_count || (description.index_stride != 2 && description.index_stride != 4);
        const bool invalid_attributes =
            description.attributes.empty() || description.attributes.size() > limits.shaderStage.vertex.attributeMaxNum;
        if (invalid_vertices || invalid_indices || invalid_attributes || description.submeshes.empty())
        {
            return cxx::unexpected(RenderError{kStorageContract});
        }
        for (std::size_t i = 0; i < description.attributes.size(); ++i)
        {
            const auto& attribute = description.attributes[i];
            const auto format = vertexFormat(attribute.format);
            const bool invalid_attribute = format.native == nri::Format::UNKNOWN ||
                                           attribute.location >= limits.shaderStage.vertex.attributeMaxNum ||
                                           attribute.offset % 4 || attribute.offset > session_.vertexOffsetLimit() ||
                                           attribute.offset > description.vertex_stride ||
                                           format.bytes() > description.vertex_stride - attribute.offset;
            if (invalid_attribute)
            {
                return cxx::unexpected(RenderError{kStorageContract});
            }
            for (std::size_t j = 0; j < i; ++j)
            {
                if (description.attributes[j].location == attribute.location)
                {
                    return cxx::unexpected(RenderError{kStorageContract});
                }
            }
            const auto supported = session_.formatSupport(format.native);
            if ((supported & nri::FormatSupportBits::VERTEX_BUFFER) == 0)
            {
                return cxx::unexpected(RenderError{kDeviceUnsupported});
            }
        }
        return {};
    }

    IndexedTriangleMeshStorage::UploadResult IndexedTriangleMeshStorage::upload(const rdesc::Mesh& mesh) noexcept
    {
        const auto owner = co_await stdexec::read_env(stdexec::get_scheduler);
        const auto stop = co_await stdexec::read_env(stdexec::get_stop_token);
        auto prepared = co_await (
            stdexec::schedule(uploads_.cpuScheduler()) | stdexec::then([&mesh]() noexcept { return packMesh(mesh); }) |
            stdexec::upon_stopped(
                []() noexcept -> RenderResult<PackedMesh> { return cxx::unexpected(RenderError{kCpuStopped}); }
            ) |
            stdexec::continues_on(owner) | stdexec::upon_error(NativeDeliveryFailure<PackedMesh>{})
        );
        if (!prepared)
        {
            co_return cxx::unexpected(prepared.error());
        }
        if (stop.stop_requested())
        {
            co_return cxx::unexpected(RenderError{kUploadStopped});
        }
        auto input = std::move(*prepared);
        if (auto valid = validate(input.description); !valid)
        {
            co_return cxx::unexpected(valid.error());
        }
        const auto vertex_bytes = input.vertices.size() * sizeof(MeshVertex);
        const auto index_bytes = std::size_t(input.description.index_count) * sizeof(std::uint32_t);
        auto vertices = vertices_.allocate(vertex_bytes, 4);
        if (!vertices)
        {
            co_return cxx::unexpected(vertices.error());
        }
        cxx::scope_exit rollback_vertices([&]() noexcept
        {
            if (!vertices_.retireAllocation(vertices->id, session_.makeFrontiers())) std::terminate();
        });
        auto indices = indices_.allocate((index_bytes + 3) & ~std::size_t{3}, 4);
        if (!indices)
        {
            co_return cxx::unexpected(indices.error());
        }
        cxx::scope_exit rollback_indices([&]() noexcept
        {
            if (!indices_.retireAllocation(indices->id, session_.makeFrontiers())) std::terminate();
        });
        std::vector<MeshGpuSection> records;
        records.reserve(input.description.submeshes.size() + input.description.lods.size());
        for (const auto& section : input.description.submeshes)
        {
            records.push_back({section.index_count, section.first_index, section.vertex_offset});
        }
        for (const auto& lod : input.description.lods)
        {
            records.push_back({lod.index_count, lod.first_index, 0});
        }
        const auto bounds_offset = (records.size() * sizeof(MeshGpuSection) + 15) & ~std::size_t{15};
        auto sections = sections_.allocate(bounds_offset + sizeof(MeshGpuBounds), 16);
        if (!sections)
        {
            co_return cxx::unexpected(sections.error());
        }
        Geometry geometry{*vertices, *indices, *sections};
        rollback_indices.release();
        rollback_vertices.release();
        cxx::scope_exit rollback([&]() noexcept { retire(geometry); });
        std::vector<BufferWrite> writes;
        writes.reserve(4 + mesh.lods.size());
        writes.push_back({geometry.vertices.slice.buffer, geometry.vertices.slice.offset, std::as_bytes(std::span(input.vertices))}
        );
        writes.push_back({geometry.indices.slice.buffer, geometry.indices.slice.offset, std::as_bytes(std::span(mesh.indices))});
        for (std::size_t i = 0; i < mesh.lods.size(); ++i)
        {
            const auto offset = std::uint64_t(input.description.lods[i].first_index) * sizeof(std::uint32_t);
            writes.push_back(
                {geometry.indices.slice.buffer,
                 geometry.indices.slice.offset + offset,
                 std::as_bytes(std::span(mesh.lods[i].indices))}
            );
        }
        writes.push_back({geometry.sections.slice.buffer, geometry.sections.slice.offset, std::as_bytes(std::span(records))});
        writes.push_back(
            {geometry.sections.slice.buffer,
             geometry.sections.slice.offset + bounds_offset,
             std::as_bytes(std::span(&input.description.bounds, 1))}
        );
        if (auto uploaded = co_await uploads_.writeBuffers(writes); !uploaded)
        {
            co_return cxx::unexpected(uploaded.error());
        }
        if (stop.stop_requested())
        {
            co_return cxx::unexpected(RenderError{kUploadStopped});
        }
        const auto key = entries_.emplace();
        auto& entry = *entries_.find(key);
        entry.geometry = std::move(geometry);
        entry.description = std::move(input.description);
        rollback.release();
        co_return extension_detail::ResourceIdentity::make<MeshId>(id_, key);
    }

    const IndexedTriangleMeshStorage::Entry* IndexedTriangleMeshStorage::findEntry(MeshId id) const noexcept
    {
        return extension_detail::ResourceIdentity::parent(id) == id_
                   ? entries_.find(extension_detail::ResourceIdentity::local(id))
                   : nullptr;
    }

    IndexedTriangleMeshStorage::Entry* IndexedTriangleMeshStorage::findEntry(MeshId id) noexcept
    {
        return const_cast<Entry*>(std::as_const(*this).findEntry(id));
    }

    const MeshDescription* IndexedTriangleMeshStorage::find(MeshId id) const noexcept
    {
        const auto* entry = findEntry(id);
        return entry ? &entry->description : nullptr;
    }

    RenderResult<IndexedTriangleMeshStorage::BufferViews>
    IndexedTriangleMeshStorage::bufferViews(MeshId id) const noexcept
    {
        const auto* entry = findEntry(id);
        if (!entry)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        const auto& vertices = entry->geometry.vertices.slice;
        const auto& indices = entry->geometry.indices.slice;
        return BufferViews{
            {vertices.buffer, vertices.offset, vertices.size},
            {indices.buffer, indices.offset, indices.size},
            entry->description.vertex_stride,
            entry->description.index_stride,
            &entry->description
        };
    }

    RenderResult<MeshArenaSection>
    IndexedTriangleMeshStorage::arenaSection(MeshId id, std::uint32_t submesh) const noexcept
    {
        const auto* entry = findEntry(id);
        if (!entry || submesh >= entry->description.submeshes.size())
            return cxx::unexpected(RenderError{kStorageIdentity});
        const auto& section = entry->description.submeshes[submesh];
        const auto index_base = entry->geometry.indices.slice.offset / entry->description.index_stride;
        const auto vertex_base = entry->geometry.vertices.slice.offset / entry->description.vertex_stride;
        const bool invalid_index = index_base > UINT32_MAX || section.first_index > UINT32_MAX - index_base;
        if (invalid_index || vertex_base > UINT32_MAX)
            return cxx::unexpected(RenderError{kStorageCapacity});
        const auto vertex_offset = static_cast<std::int64_t>(vertex_base) + section.vertex_offset;
        if (vertex_offset < INT32_MIN || vertex_offset > INT32_MAX)
            return cxx::unexpected(RenderError{kStorageCapacity});
        return MeshArenaSection{
            {section.index_count, static_cast<std::uint32_t>(index_base + section.first_index),
                static_cast<std::int32_t>(vertex_offset)},
            entry->description.bounds
        };
    }

    RenderResult<void> IndexedTriangleMeshStorage::release(MeshId id) noexcept
    {

        auto* entry = findEntry(id);
        if (!entry)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        retire(entry->geometry);
        entries_.erase(extension_detail::ResourceIdentity::local(id));
        return {};
    }

    void IndexedTriangleMeshStorage::retire(const Geometry& geometry) noexcept
    {
        // The exclusive release excludes prepared frames. Upload rollback reaches this point after its native receipt.
        auto uses = session_.makeFrontiers();
        for (std::size_t queue = 0; queue < session_.queueCount(); ++queue)
        {
            uses.accept(session_.submissionPoint(queue, session_.physicalQueue(queue).submitted()));
        }
        const bool vertices_retired = vertices_.retireAllocation(geometry.vertices.id, uses);
        const bool indices_retired = indices_.retireAllocation(geometry.indices.id, uses);
        const bool sections_retired = sections_.retireAllocation(geometry.sections.id, std::move(uses));
        if (!(vertices_retired && indices_retired && sections_retired)) std::terminate();
    }

    ResourceRegistration IndexedTriangleMeshStorage::factory() noexcept
    {

        static const std::array commands{
            CommandBinding::declare<MeshCommands::UploadMesh, &createMesh>(),
            CommandBinding::declare<MeshCommands::ReleaseMesh, &releaseMesh>()
        };
        static const auto create = +[](ResourceCreateContext& dependencies, ResourceInstanceId id
                                    ) noexcept -> RenderResult<std::unique_ptr<IndexedTriangleMeshStorage>>
        {
            if (!dependencies.uploads)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            return std::make_unique<IndexedTriangleMeshStorage>(dependencies, id);
        };
        return makeResourceRegistration<render::MeshResource, IndexedTriangleMeshStorage, create>(
            nullptr,
            nullptr,
            commands
        );
    }
} // namespace lux::render::detail

namespace lux::render
{
    RenderResult<MeshDescription>
    readMeshLayout(const RasterProgramBuildContext& selection, DrawStreamSlot slot, MeshId id) noexcept
    {
        const auto resource = extension_detail::ResourceIdentity::parent(id);
        auto storage = selection.readResource<detail::IndexedTriangleMeshStorage>(slot, resource);
        if (!storage) return cxx::unexpected(storage.error());
        const auto* layout = (*storage)->find(id);
        if (!layout) return cxx::unexpected(RenderError{kStorageIdentity, {slot.index}});
        return *layout;
    }
} // namespace lux::render

namespace lux::render
{
    DrawResourceType MeshResource::drawResourceType() noexcept
    {
        return {ResourceName{kMeshResourceName.name()}, cxx::make_basic_type_info<detail::IndexedTriangleMeshStorage>()};
    }
}
