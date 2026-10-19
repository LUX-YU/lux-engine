#include "BufferArena.hpp"
#include "Shader.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/native/vulkan/CompiledGraph.hpp>
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/mesh/MeshStorage.hpp>

namespace lux::render::vulkan
{
    struct MeshStorage::Geometry final
    {
        std::unique_ptr<BufferArena::Range> vertices, indices, sections;
        RetirementReservation vertex_retirement, index_retirement, section_retirement;
    };

    MeshStorage::MeshStorage(DeviceSession& session, StorageId id) noexcept
        : session_(session), id_(id),
          vertices_(std::make_unique<BufferArena>(session, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)),
          indices_(std::make_unique<BufferArena>(session, VK_BUFFER_USAGE_INDEX_BUFFER_BIT)),
          sections_(std::make_unique<BufferArena>(session, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 65536))
    {
    }

    MeshStorage::~MeshStorage() noexcept
    {
        for (auto& entry : entries_)
        {
            disconnect(entry);
            retire(entry.geometry);
        }
    }

    std::size_t MeshStorage::recordCount() const noexcept
    {
        return entries_.size();
    }

    RenderResult<void> MeshStorage::validate(const MeshDescription& description, std::span<const std::byte> source)
        const noexcept
    {
        const auto& limits = session_.physicalDevice().properties().properties.limits;
        const bool invalid_vertices = !description.vertex_count || !description.vertex_stride ||
                                      description.vertex_stride > limits.maxVertexInputBindingStride ||
                                      description.vertex_stride % 4;
        const bool invalid_indices =
            !description.index_count || (description.index_stride != 2 && description.index_stride != 4);
        const bool invalid_attributes =
            description.attributes.empty() || description.attributes.size() > limits.maxVertexInputAttributes;
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
            const bool invalid_attribute =
                format.native == VK_FORMAT_UNDEFINED || attribute.location >= limits.maxVertexInputAttributes ||
                attribute.offset % 4 || attribute.offset > limits.maxVertexInputAttributeOffset ||
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
            const auto supported = session_.physicalDevice().formatProperties(format.native);
            if (!(supported.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT))
            {
                return cxx::unexpected(RenderError{kUnsupported});
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

    MeshStorage::UploadResult MeshStorage::upload(MeshWrite input) noexcept
    {
        if (input.mesh.storage != id_)
        {
            co_return cxx::unexpected(RenderError{kStorageIdentity});
        }
        auto* existing = input.mesh.isNull() ? nullptr : entries_.find(input.mesh.local);
        if (!input.mesh.isNull() && !existing)
        {
            co_return cxx::unexpected(RenderError{kStorageIdentity});
        }
        if (existing && existing->updating)
        {
            co_return cxx::unexpected(RenderError{kBusy});
        }
        const auto revision = existing ? existing->revision : 0;
        if (input.expected_revision != revision || revision == UINT64_MAX)
        {
            co_return cxx::unexpected(RenderError{kStorageContract});
        }
        if (auto valid = validate(input.description, input.source); !valid)
        {
            co_return cxx::unexpected(valid.error());
        }
        const bool has_bindings = existing && (existing->vertices || existing->indices || existing->sections);
        if (has_bindings && existing->description != input.description)
        {
            co_return cxx::unexpected(RenderError{kStorageMismatch});
        }
        const auto vertex_bytes = std::size_t(input.description.vertex_count) * input.description.vertex_stride;
        const auto index_bytes = std::size_t(input.description.index_count) * input.description.index_stride;
        auto vertices = vertices_->allocate(vertex_bytes, 4);
        if (!vertices)
        {
            co_return cxx::unexpected(vertices.error());
        }
        auto indices = indices_->allocate((index_bytes + 3) & ~std::size_t{3}, 4);
        if (!indices)
        {
            co_return cxx::unexpected(indices.error());
        }
        std::vector<MeshGpuSection> records;
        records.reserve(input.description.submeshes.size());
        for (const auto& section : input.description.submeshes)
        {
            records.push_back({section.index_count, section.first_index, section.vertex_offset});
        }
        auto sections = sections_->allocate(records.size() * sizeof(MeshGpuSection), 4);
        if (!sections)
        {
            co_return cxx::unexpected(sections.error());
        }
        auto geometry = std::make_unique<Geometry>(
            std::move(*vertices),
            std::move(*indices),
            std::move(*sections),
            session_.retirement().reserve(),
            session_.retirement().reserve(),
            session_.retirement().reserve()
        );
        auto id = input.mesh;
        if (id.isNull())
        {
            id.local = entries_.emplace();
        }
        auto& entry = *entries_.find(id.local); // StableSlotMap retains this address across suspension.
        entry.updating = true;
        cxx::scope_exit rollback(
            [&]() noexcept
            {
                entry.updating = false;
                retire(geometry);
                if (!entry.geometry)
                {
                    entries_.erase(id.local);
                }
            }
        );
        auto uploaded = co_await session_.uploads().writeBufferRange(
            *geometry->vertices->buffer,
            geometry->vertices->offset,
            input.source.first(vertex_bytes)
        );
        if (!uploaded)
        {
            co_return cxx::unexpected(uploaded.error());
        }
        const auto index_source = input.source.subspan(input.description.index_offset, index_bytes);
        const auto full_words = index_bytes & ~std::size_t{3};
        if (full_words)
        {
            uploaded = co_await session_.uploads().writeBufferRange(
                *geometry->indices->buffer,
                geometry->indices->offset,
                index_source.first(full_words)
            );
            if (!uploaded)
            {
                co_return cxx::unexpected(uploaded.error());
            }
        }
        if (full_words != index_bytes)
        {
            std::array<std::byte, 4> tail{};
            std::memcpy(tail.data(), index_source.data() + full_words, index_bytes - full_words);
            uploaded = co_await session_.uploads()
                           .writeBufferRange(*geometry->indices->buffer, geometry->indices->offset + full_words, tail);
            if (!uploaded)
            {
                co_return cxx::unexpected(uploaded.error());
            }
        }
        const auto stop = co_await stdexec::read_env(stdexec::get_stop_token);
        std::optional<CommandBatch> batch;
        for (;;)
        {
            if (stop.stop_requested())
            {
                co_return cxx::unexpected(RenderError{kWorkStopped});
            }
            auto ready = session_.queue().begin();
            if (ready)
            {
                batch.emplace(std::move(*ready));
                break;
            }
            if (ready.error().type != kCapacity)
            {
                co_return cxx::unexpected(ready.error());
            }
            auto completed = co_await session_.queue().whenAvailable();
            if (!completed)
            {
                co_return cxx::unexpected(completed.error());
            }
        }
        const auto bytes = std::as_bytes(std::span(records));
        const auto& section_range = *geometry->sections;
        for (std::size_t offset = 0; offset < bytes.size(); offset += 65536)
        {
            vkCmdUpdateBuffer(
                batch->native(),
                section_range.buffer->native(),
                section_range.offset + offset,
                std::min<std::size_t>(65536, bytes.size() - offset),
                bytes.data() + offset
            );
        }
        VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(batch->native(), &dependency);
        if (auto accepted = std::move(*batch).submit(); !accepted)
        {
            co_return cxx::unexpected(accepted.error());
        }
        retire(entry.geometry);
        entry.geometry = std::move(geometry);
        entry.description = std::move(input.description);
        entry.revision = revision + 1;
        entry.updating = false;
        const auto& v = *entry.geometry->vertices;
        const auto& i = *entry.geometry->indices;
        StorageSubscription::publish(entry.vertices, {v.buffer, v.offset, v.size});
        StorageSubscription::publish(entry.indices, {i.buffer, i.offset, i.size});
        const auto& s = *entry.geometry->sections;
        StorageSubscription::publish(entry.sections, {s.buffer, s.offset, s.size});
        rollback.release();
        co_return MeshPublished{id, entry.revision};
    }

    const MeshDescription* MeshStorage::find(MeshId id) const noexcept
    {
        const auto* entry = id.storage == id_ ? entries_.find(id.local) : nullptr;
        return entry && entry->geometry ? &entry->description : nullptr;
    }

    RenderResult<MeshInformation> MeshStorage::query(MeshId id) const noexcept
    {
        const auto* entry = id.storage == id_ ? entries_.find(id.local) : nullptr;
        if (!entry || !entry->geometry)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        return MeshInformation{
            {id, entry->revision},
            entry->description,
            entry->geometry->vertices->size + entry->geometry->indices->size
        };
    }

    RenderResult<void> MeshStorage::release(MeshId id, std::uint64_t revision) noexcept
    {
        auto* entry = id.storage == id_ ? entries_.find(id.local) : nullptr;
        if (!entry || !entry->geometry)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        if (entry->updating)
        {
            return cxx::unexpected(RenderError{kBusy});
        }
        if (revision != entry->revision)
        {
            return cxx::unexpected(RenderError{kStorageContract});
        }
        disconnect(*entry);
        retire(entry->geometry);
        entries_.erase(id.local);
        return {};
    }

    RenderResult<StorageResource> MeshStorage::resolve(
        const NativeResourcePlan& plan,
        const MeshBinding& argument,
        StorageSubscription& subscription
    ) noexcept
    {
        auto* entry = argument.mesh.storage == id_ ? entries_.find(argument.mesh.local) : nullptr;
        if (!entry || !entry->geometry)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        const auto* description = std::get_if<GraphBufferDesc>(&plan.description);
        const bool is_invalid_kind = argument.buffer > EMeshBuffer::SECTIONS;
        const bool is_mismatch = !description || argument.description != entry->description || is_invalid_kind;
        if (is_mismatch)
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        const bool vertex = argument.buffer == EMeshBuffer::VERTICES;
        const bool index = argument.buffer == EMeshBuffer::INDICES;
        const auto& range = vertex  ? *entry->geometry->vertices
                            : index ? *entry->geometry->indices
                                    : *entry->geometry->sections;
        if (description->size > range.size)
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        subscription.attach(vertex ? entry->vertices : index ? entry->indices : entry->sections);
        return StorageResource{range.buffer, range.offset, range.size};
    }

    void MeshStorage::retire(std::unique_ptr<Geometry>& geometry) noexcept
    {
        if (!geometry)
        {
            return;
        }
        if (!BufferArena::retireRange(session_.retirement(), geometry->vertex_retirement, geometry->vertices) ||
            !BufferArena::retireRange(session_.retirement(), geometry->index_retirement, geometry->indices) ||
            !BufferArena::retireRange(session_.retirement(), geometry->section_retirement, geometry->sections))
        {
            std::terminate(); // Both reservations precede any accepted GPU work.
        }
        geometry.reset();
    }

    void MeshStorage::disconnect(Entry& entry) noexcept
    {
        StorageSubscription::disconnect(entry.vertices);
        StorageSubscription::disconnect(entry.indices);
        StorageSubscription::disconnect(entry.sections);
    }

    StorageFactory meshStorageFactory() noexcept
    {
        static const render::MeshStorage::Ops ops{
            [](const void* object, MeshId id) noexcept { return static_cast<const MeshStorage*>(object)->query(id); },
            [](void* object, MeshId id, std::uint64_t revision) noexcept
            { return static_cast<MeshStorage*>(object)->release(id, revision); }
        };
        static const StorageBindingOps binding{
            [](void* object, const NativeResourcePlan& plan, const void* argument, StorageSubscription& subscription
            ) noexcept
            {
                return static_cast<MeshStorage*>(object)
                    ->resolve(plan, *static_cast<const MeshBinding*>(argument), subscription);
            }
        };
        static const auto create =
            +[](DeviceSession& session, StorageId id) noexcept -> RenderResult<std::unique_ptr<MeshStorage>>
        { return std::make_unique<MeshStorage>(session, id); };
        return makeStorageFactory<render::MeshStorage, MeshStorage, DeviceSession, create>(ops, &binding);
    }
} // namespace lux::render::vulkan

namespace lux::render
{
    StorageTraits<vulkan::MeshStorage>::Result StorageTraits<vulkan::MeshStorage>::upload(
        vulkan::MeshStorage& storage,
        MeshWrite input
    ) noexcept
    {
        return storage.upload(std::move(input));
    }
} // namespace lux::render
