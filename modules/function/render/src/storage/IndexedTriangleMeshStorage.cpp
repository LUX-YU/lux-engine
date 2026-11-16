#include <lux/engine/render/storage/IndexedTriangleMeshCommandHandler.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/graph/RenderPlan.hpp>
#include <lux/engine/render/graph/Shader.hpp>
#include <lux/engine/render/memory/BufferArena.hpp>
#include <lux/engine/render/storage/IndexedTriangleMeshStorage.hpp>
#include <lux/engine/render/storage/MeshPacking.hpp>

namespace lux::render::detail
{
    IndexedTriangleMeshStorage::IndexedTriangleMeshStorage(ResourceCreateContext& dependencies, ResourceInstanceId id) noexcept
        : session_(dependencies.device), uploads_(*dependencies.uploads), id_(id),
          vertices_(dependencies.device, nri::BufferUsageBits::VERTEX),
          indices_(dependencies.device, nri::BufferUsageBits::INDEX),
          sections_(dependencies.device, (nri::BufferUsageBits::SHADER_RESOURCE | nri::BufferUsageBits::SHADER_RESOURCE_STORAGE), 65536)
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
        auto prepared = co_await (
            stdexec::schedule(uploads_.cpuScheduler()) | stdexec::then([&mesh]() noexcept { return packMesh(mesh); }) |
            stdexec::upon_stopped(
                []() noexcept -> RenderResult<PackedMesh> { return cxx::unexpected(RenderError{kCpuStopped}); }
            )
        );
        if (!prepared)
        {
            co_return cxx::unexpected(prepared.error());
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
        cxx::scope_exit rollback_vertices([&]() noexcept { vertices_.release(*vertices); });
        auto indices = indices_.allocate((index_bytes + 3) & ~std::size_t{3}, 4);
        if (!indices)
        {
            co_return cxx::unexpected(indices.error());
        }
        cxx::scope_exit rollback_indices([&]() noexcept { indices_.release(*indices); });
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
        Geometry geometry{std::move(*vertices), std::move(*indices), std::move(*sections)};
        cxx::scope_exit rollback([&]() noexcept { retire(geometry); });
        std::vector<BufferWrite> writes;
        writes.reserve(4 + mesh.lods.size());
        writes.push_back({geometry.vertices.buffer, geometry.vertices.offset, std::as_bytes(std::span(input.vertices))}
        );
        writes.push_back({geometry.indices.buffer, geometry.indices.offset, std::as_bytes(std::span(mesh.indices))});
        for (std::size_t i = 0; i < mesh.lods.size(); ++i)
        {
            const auto offset = std::uint64_t(input.description.lods[i].first_index) * sizeof(std::uint32_t);
            writes.push_back(
                {geometry.indices.buffer,
                 geometry.indices.offset + offset,
                 std::as_bytes(std::span(mesh.lods[i].indices))}
            );
        }
        writes.push_back({geometry.sections.buffer, geometry.sections.offset, std::as_bytes(std::span(records))});
        writes.push_back(
            {geometry.sections.buffer,
             geometry.sections.offset + bounds_offset,
             std::as_bytes(std::span(&input.description.bounds, 1))}
        );
        if (auto uploaded = co_await uploads_.writeBuffers(writes); !uploaded)
        {
            co_return cxx::unexpected(uploaded.error());
        }
        const auto key = entries_.emplace();
        auto& entry = *entries_.find(key);
        entry.geometry = std::move(geometry);
        entry.description = std::move(input.description);
        co_return extension_detail::ResourceIdentity::make<MeshId>(id_, key);
    }

    const MeshDescription* IndexedTriangleMeshStorage::find(MeshId id) const noexcept
    {
        const auto* entry = extension_detail::ResourceIdentity::storage(id) == id_
                                ? entries_.find(extension_detail::ResourceIdentity::local(id))
                                : nullptr;
        return entry ? &entry->description : nullptr;
    }

    RenderResult<IndexedTriangleMeshStorage::BufferViews>
    IndexedTriangleMeshStorage::bufferViews(MeshId id) const noexcept
    {
        const auto* entry = extension_detail::ResourceIdentity::storage(id) == id_
                                ? entries_.find(extension_detail::ResourceIdentity::local(id))
                                : nullptr;
        if (!entry)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        const auto& vertices = entry->geometry.vertices;
        const auto& indices = entry->geometry.indices;
        return BufferViews{
            {vertices.buffer, vertices.offset, vertices.size},
            {indices.buffer, indices.offset, indices.size},
            entry->description.vertex_stride, entry->description.index_stride
        };
    }

    RenderResult<void> IndexedTriangleMeshStorage::release(MeshId id) noexcept
    {
        if (uploads_.resourcesFrozen())
        {
            return cxx::unexpected(RenderError{kDeviceBusy});
        }
        auto* entry = extension_detail::ResourceIdentity::storage(id) == id_
                          ? entries_.find(extension_detail::ResourceIdentity::local(id))
                          : nullptr;
        if (!entry)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        retire(entry->geometry);
        entries_.erase(extension_detail::ResourceIdentity::local(id));
        return {};
    }

    RenderResult<ResourceInput> IndexedTriangleMeshStorage::
        resolve(const VGraphResourceDesc& plan, const MeshBinding& argument, StorageSubscription&) noexcept
    {
        auto* entry = extension_detail::ResourceIdentity::storage(argument.mesh) == id_
                          ? entries_.find(extension_detail::ResourceIdentity::local(argument.mesh))
                          : nullptr;
        if (!entry)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        const auto* description = std::get_if<GraphBufferDesc>(&plan);
        const bool is_invalid_kind = argument.buffer > EMeshBuffer::BOUNDS;
        const bool is_mismatch = !description || is_invalid_kind;
        if (is_mismatch)
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        const bool vertex = argument.buffer == EMeshBuffer::VERTICES;
        const bool index = argument.buffer == EMeshBuffer::INDICES;
        const bool bounds = argument.buffer == EMeshBuffer::BOUNDS;
        const bool is_layout_mismatch = vertex ? argument.stride != entry->description.vertex_stride ||
                                                     argument.attributes != entry->description.attributes
                                               : argument.stride != (index    ? entry->description.index_stride
                                                                     : bounds ? sizeof(MeshGpuBounds)
                                                                              : sizeof(MeshGpuSection));
        if (is_layout_mismatch)
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        const auto& range = vertex  ? entry->geometry.vertices
                            : index ? entry->geometry.indices
                                    : entry->geometry.sections;
        const auto section_count = entry->description.submeshes.size() + entry->description.lods.size();
        const auto section_bytes = section_count * sizeof(MeshGpuSection);
        const auto offset = bounds ? (section_bytes + 15) & ~std::size_t{15} : 0;
        const auto size = bounds                                     ? sizeof(MeshGpuBounds)
                          : argument.buffer == EMeshBuffer::SECTIONS ? section_bytes
                                                                     : range.size;
        if (description->size > size)
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        return ResourceInput{range.buffer, range.offset + offset, size};
    }

    void IndexedTriangleMeshStorage::retire(Geometry& geometry) noexcept
    {
        vertices_.release(geometry.vertices);
        indices_.release(geometry.indices);
        sections_.release(geometry.sections);
    }

    ResourceRegistration IndexedTriangleMeshStorage::factory() noexcept
    {

        static const std::array commands{
            CommandBinding::declare<Mesh3dCommands::UploadMesh, &IndexedTriangleMeshCommandHandler::createMesh>(),
            CommandBinding::declare<Mesh3dCommands::ReleaseMesh, &IndexedTriangleMeshCommandHandler::releaseMesh>()
        };
        static const StorageBindingOps binding{
            [](void* object,
               const VGraphResourceDesc& plan,
               const void* argument,
               StorageSubscription& subscription,
               std::span<ResourceInput> output) noexcept -> RenderResult<void>
            {
                auto value = static_cast<IndexedTriangleMeshStorage*>(object)
                                 ->resolve(plan, *static_cast<const MeshBinding*>(argument), subscription);
                if (!value)
                {
                    return cxx::unexpected(value.error());
                }
                std::ranges::fill(output, *value);
                return {};
            }
        };
        static const auto create = +[](ResourceCreateContext& dependencies, ResourceInstanceId id
                                    ) noexcept -> RenderResult<std::unique_ptr<IndexedTriangleMeshStorage>>
        {
            if (!dependencies.uploads || dependencies.frame_count == 0 || dependencies.frame_count > 3)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            return std::make_unique<IndexedTriangleMeshStorage>(dependencies, id);
        };
        return makeResourceRegistration<
            render::MeshResource,
            IndexedTriangleMeshStorage,
            create>(nullptr, &binding,
            commands
        );
    }
} // namespace lux::render::detail

namespace lux::render
{
    StorageTraits<detail::IndexedTriangleMeshStorage>::Result StorageTraits<detail::IndexedTriangleMeshStorage>::upload(
        detail::IndexedTriangleMeshStorage& storage,
        const rdesc::Mesh& input
    ) noexcept
    {
        return storage.upload(input);
    }
} // namespace lux::render
