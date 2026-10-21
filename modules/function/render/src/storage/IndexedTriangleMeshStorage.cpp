#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/graph/RenderPlan.hpp>
#include <lux/engine/render/graph/Shader.hpp>
#include <lux/engine/render/memory/BufferArena.hpp>
#include <lux/engine/render/storage/IndexedTriangleMeshStorage.hpp>

namespace lux::render::detail
{
    IndexedTriangleMeshStorage::IndexedTriangleMeshStorage(StorageCreateContext& dependencies, StorageId id) noexcept
        : session_(dependencies.device), uploads_(*dependencies.uploads), frames_(*dependencies.frames), id_(id),
          vertices_(dependencies.device, nri::BufferUsageBits::VERTEX_BUFFER),
          indices_(dependencies.device, nri::BufferUsageBits::INDEX_BUFFER),
          sections_(dependencies.device, nri::BufferUsageBits::SHADER_RESOURCE_STORAGE, 65536)
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

    RenderResult<void> IndexedTriangleMeshStorage::validate(
        const MeshDescription& description,
        std::span<const std::byte> source
    ) const noexcept
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
        const std::uint64_t vertex_bytes = std::uint64_t(description.vertex_count) * description.vertex_stride;
        const std::uint64_t index_bytes = std::uint64_t(description.index_count) * description.index_stride;
        const auto total = cxx::checkedAdd(description.index_offset, index_bytes);
        if (!total)
        {
            return cxx::unexpected(RenderError{kStorageContract});
        }
        const auto padded = cxx::checkedAdd(*total, std::uint64_t{3});
        const bool invalid_range = description.index_offset < vertex_bytes ||
                                   description.index_offset % description.index_stride || !padded ||
                                   (*padded & ~std::uint64_t{3}) != source.size();
        if (invalid_range)
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
        for (const auto& submesh : description.submeshes)
        {
            const bool invalid_submesh = !submesh.index_count || submesh.index_count % 3 ||
                                         submesh.first_index > description.index_count ||
                                         submesh.index_count > description.index_count - submesh.first_index;
            if (invalid_submesh)
            {
                return cxx::unexpected(RenderError{kStorageContract});
            }
            for (std::uint32_t i = 0; i < submesh.index_count; ++i)
            {
                const auto* bytes = source.data() + description.index_offset +
                                    std::uint64_t(submesh.first_index + i) * description.index_stride;
                std::uint32_t index = 0;
                if (description.index_stride == 2)
                {
                    std::uint16_t narrow = 0;
                    std::memcpy(&narrow, bytes, sizeof(narrow));
                    index = narrow;
                }
                else
                {
                    std::memcpy(&index, bytes, sizeof(index));
                }
                const auto vertex = std::int64_t(index) + submesh.vertex_offset;
                if (vertex < 0 || vertex >= description.vertex_count)
                {
                    return cxx::unexpected(RenderError{kStorageContract});
                }
            }
        }
        return {};
    }

    IndexedTriangleMeshStorage::UploadResult IndexedTriangleMeshStorage::upload(MeshWrite input) noexcept
    {
        if (input.storage != id_)
        {
            co_return cxx::unexpected(RenderError{kStorageIdentity});
        }
        if (auto valid = validate(input.description, input.source); !valid)
        {
            co_return cxx::unexpected(valid.error());
        }
        const auto vertex_bytes = std::size_t(input.description.vertex_count) * input.description.vertex_stride;
        const auto index_bytes = std::size_t(input.description.index_count) * input.description.index_stride;
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
        records.reserve(input.description.submeshes.size());
        for (const auto& section : input.description.submeshes)
        {
            records.push_back({section.index_count, section.first_index, section.vertex_offset});
        }
        const auto bounds_offset = (records.size() * sizeof(MeshGpuSection) + 15) & ~std::size_t{15};
        auto sections = sections_.allocate(bounds_offset + sizeof(MeshGpuBounds), 16);
        if (!sections)
        {
            co_return cxx::unexpected(sections.error());
        }
        Geometry geometry{std::move(*vertices), std::move(*indices), std::move(*sections)};
        cxx::scope_exit rollback([&]() noexcept { retire(geometry); });
        std::array<BufferWrite, 5> writes;
        std::size_t count = 0;
        writes[count++] = {geometry.vertices.buffer, geometry.vertices.offset, input.source.first(vertex_bytes)};
        const auto index_source = input.source.subspan(input.description.index_offset, index_bytes);
        const auto full_words = index_bytes & ~std::size_t{3};
        if (full_words)
        {
            writes[count++] = {geometry.indices.buffer, geometry.indices.offset, index_source.first(full_words)};
        }
        std::array<std::byte, 4> tail{};
        if (full_words != index_bytes)
        {
            std::memcpy(tail.data(), index_source.data() + full_words, index_bytes - full_words);
            writes[count++] = {geometry.indices.buffer, geometry.indices.offset + full_words, tail};
        }
        writes[count++] = {geometry.sections.buffer, geometry.sections.offset, std::as_bytes(std::span(records))};
        writes[count++] = {
            geometry.sections.buffer,
            geometry.sections.offset + bounds_offset,
            std::as_bytes(std::span(&input.description.bounds, 1))
        };
        if (auto uploaded = co_await uploads_.writeBuffers(std::span(writes).first(count)); !uploaded)
        {
            co_return cxx::unexpected(uploaded.error());
        }
        const auto key = entries_.emplace();
        auto& entry = *entries_.find(key);
        entry.geometry = std::move(geometry);
        entry.description = std::move(input.description);
        co_return MeshId{id_, key};
    }

    const MeshDescription* IndexedTriangleMeshStorage::find(MeshId id) const noexcept
    {
        const auto* entry = id.storage == id_ ? entries_.find(id.local) : nullptr;
        return entry ? &entry->description : nullptr;
    }

    RenderResult<void> IndexedTriangleMeshStorage::release(MeshId id) noexcept
    {
        if (frames_.submitting())
        {
            return cxx::unexpected(RenderError{kDeviceBusy});
        }
        auto* entry = id.storage == id_ ? entries_.find(id.local) : nullptr;
        if (!entry)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        retire(entry->geometry);
        entries_.erase(id.local);
        return {};
    }

    RenderResult<ResourceInput> IndexedTriangleMeshStorage::
        resolve(const CompiledResource& plan, const MeshBinding& argument, StorageSubscription&) noexcept
    {
        auto* entry = argument.mesh.storage == id_ ? entries_.find(argument.mesh.local) : nullptr;
        if (!entry)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        const auto* description = std::get_if<GraphBufferDesc>(&plan.description);
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
        const auto offset =
            bounds ? (entry->description.submeshes.size() * sizeof(MeshGpuSection) + 15) & ~std::size_t{15} : 0;
        const auto size = bounds ? sizeof(MeshGpuBounds)
                          : argument.buffer == EMeshBuffer::SECTIONS
                              ? entry->description.submeshes.size() * sizeof(MeshGpuSection)
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

    StorageFactory meshStorageFactory() noexcept
    {
        static const StorageBindingOps binding{
            [](void* object,
               const CompiledResource& plan,
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
        static const auto create = +[](StorageCreateContext& dependencies, StorageId id
                                    ) noexcept -> RenderResult<std::unique_ptr<IndexedTriangleMeshStorage>>
        {
            if (!dependencies.uploads || !dependencies.frames)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            return std::make_unique<IndexedTriangleMeshStorage>(dependencies, id);
        };
        return makeStorageFactory<
            render::IndexedTriangleMeshStorageDescriptor,
            IndexedTriangleMeshStorage,
            StorageCreateContext,
            create>(nullptr, &binding);
    }
} // namespace lux::render::detail

namespace lux::render
{
    StorageTraits<detail::IndexedTriangleMeshStorage>::Result StorageTraits<detail::IndexedTriangleMeshStorage>::upload(
        detail::IndexedTriangleMeshStorage& storage,
        MeshWrite input
    ) noexcept
    {
        return storage.upload(std::move(input));
    }
} // namespace lux::render
