#include <bit>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/features/RasterMeshDraws.hpp>
#include <lux/engine/render/graph/RenderPlan.hpp>
#include <lux/engine/render/memory/Cleanup.hpp>
#include <lux/engine/render/storage/IndexedTriangleMeshStorage.hpp>
#include <lux/engine/render/storage/RasterMaterialTableStorage.hpp>
#include <lux/engine/render/upload/ResourceUploader.hpp>

namespace lux::render::detail
{
    RasterMeshDraws::RasterMeshDraws(
        DeviceSession& session,
        FrameExecution& frames,
        ResourceRegistry& resources,
        MeshInstanceDrawGroup selection
    ) noexcept
        : session_(session), frames_(frames), resources_(resources), selection_(selection), words_(5)
    {
    }

    RasterMeshDraws::~RasterMeshDraws() noexcept
    {
        observer_.reset();
        for (auto& slice : gpu_)
        {
            releaseAfterGpu(session_, slice.buffer);
        }
    }

    bool RasterMeshDraws::selected(const RasterMeshInstanceStorage::Selection& input) const noexcept
    {
        const bool different_geometry = input.mesh != selection_.mesh || input.submesh != selection_.submesh;
        const bool different_material_storage = input.material.storage != selection_.material_table_source.storage;
        if (different_geometry || different_material_storage)
        {
            return false;
        }
        auto material = resources_.borrow<RasterMaterialTableStorage>(selection_.material_table_source.storage);
        return material && (*material)->sameTable(input.material, selection_.material_table_source);
    }

    void RasterMeshDraws::update(MeshInstanceId id, const RasterMeshInstanceStorage::Selection* description) noexcept
    {
        if (id.isNull())
        {
            available_ = false;
            return;
        }
        const auto index = id.local.index;
        const bool present = index < positions_.size() && positions_[index] != UINT32_MAX;
        const bool include = description && selected(*description);
        if (present == include)
        {
            return;
        }
        if (include)
        {
            if (index >= positions_.size())
            {
                positions_.resize(std::bit_ceil(std::size_t(index) + 1), UINT32_MAX);
            }
            positions_[index] = words_[1]++;
            words_.push_back(index);
        }
        else
        {
            const auto position = positions_[index];
            const auto last = words_.back();
            words_[5 + position] = last;
            positions_[last] = position;
            positions_[index] = UINT32_MAX;
            words_.pop_back();
            --words_[1];
        }
        ++revision_;
        // Preparation may have uploaded only a prefix. A changed candidate set invalidates that
        // prefix, including its count header; it is not a continuation of the previous revision.
        for (auto& slice : gpu_)
        {
            slice.uploaded_bytes = 0;
        }
    }

    RenderResult<FeatureBinding::Preparation> RasterMeshDraws::prepare(
        std::uint32_t slot,
        ResourceUploader& uploads
    ) noexcept
    {
        if (!available_)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        if (words_[1] > capacity_)
        {
            return cxx::unexpected(RenderError{kStorageCapacity});
        }
        auto& slice = gpu_[slot];
        slice.pending_revision = 0;
        const auto prepared = [&](FeatureBinding::EPreparation state) noexcept
        {
            FeatureBinding::Preparation result{state};
            if (std::exchange(slice.binding_changed, false))
            {
                update_.input = ResourceInput{slice.buffer, 0, session_.nriCore().GetBufferDesc(*slice.buffer).size};
                result.resources = std::span(&update_, 1);
            }
            if (state == FeatureBinding::EPreparation::RECORD)
            {
                result.copies = slice.copies;
            }
            return result;
        };
        if (slice.uploaded_revision == revision_)
        {
            return prepared(FeatureBinding::EPreparation::READY);
        }
        const auto bytes = std::max<std::size_t>(24, words_.size() * sizeof(std::uint32_t));
        if ((!slice.buffer || session_.nriCore().GetBufferDesc(*slice.buffer).size < bytes))
        {
            const auto capacity = std::bit_ceil(bytes);
            if (capacity > session_.description().memory.storageBufferMaxRange)
            {
                return cxx::unexpected(RenderError{kStorageCapacity});
            }
            const nri::BufferDesc description{
                capacity,
                4,
                nri::BufferUsageBits::SHADER_RESOURCE_STORAGE | nri::BufferUsageBits::ARGUMENT_BUFFER
            };
            nri::Buffer* buffer = nullptr;
            const auto created =
                session_.nriCore().CreatePlacedBuffer(session_.device(), NriDeviceHeap, description, buffer);
            if (created != nri::Result::SUCCESS)
            {
                return cxx::unexpected(nativeError(created));
            }
            releaseAfterGpu(session_, slice.buffer);
            slice.buffer = buffer;
            slice.uploaded_bytes = 0;
            slice.binding_changed = true;
        }
        const auto source = std::as_bytes(std::span(words_));
        slice.pending_bytes = slice.uploaded_bytes;
        cxx::scope_exit register_ack(
            [&]() noexcept
            {
                if (slice.pending_bytes == slice.uploaded_bytes && !slice.pending_revision)
                {
                    return;
                }
                frames_.onPreparationSubmitted(
                    &slice,
                    [](void* value, std::uint32_t, RenderResult<void> accepted) noexcept
                    {
                        auto& slice = *static_cast<GpuSlice*>(value);
                        if (accepted)
                        {
                            slice.uploaded_bytes = slice.pending_bytes;
                            if (slice.pending_revision)
                            {
                                slice.uploaded_revision = slice.pending_revision;
                                slice.uploaded_bytes = 0;
                            }
                        }
                        slice.pending_revision = 0;
                    }
                );
            }
        );
        while (slice.pending_bytes < source.size())
        {
            auto copied = uploads.stageBuffer({slice.buffer, slice.pending_bytes, source.subspan(slice.pending_bytes)});
            if (!copied)
            {
                return cxx::unexpected(copied.error());
            }
            if (*copied == 0)
            {
                return prepared(FeatureBinding::EPreparation::WAITING);
            }
            slice.pending_bytes += *copied;
        }
        slice.pending_revision = revision_;
        const auto section = sections_.offset + std::uint64_t(selection_.submesh) * sizeof(MeshGpuSection);
        slice.copies = {
            {{sections_.buffer, slice.buffer, section, 0, 4},
             {sections_.buffer, slice.buffer, section + 4, 8, 4},
             {sections_.buffer, slice.buffer, section + 8, 12, 4}}
        };
        return prepared(FeatureBinding::EPreparation::RECORD);
    }

    RenderResult<FeatureBinding> RasterMeshDraws::create(
        DeviceSession& session,
        FrameExecution& frames,
        ResourceRegistry& registry,
        std::span<const StorageId> storages,
        const void* argument
    ) noexcept
    {
        const auto& configuration = *static_cast<const MeshDrawConfiguration*>(argument);
        const auto& group = configuration.selection;
        auto instances = registry.borrow<RasterMeshInstanceStorage>(storages[configuration.instances.index]);
        auto mesh = registry.borrow<IndexedTriangleMeshStorage>(group.mesh.storage);
        auto material = registry.borrow<RasterMaterialTableStorage>(group.material_table_source.storage);
        if (!instances || !mesh || !material)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        const auto* description = (*mesh)->find(group.mesh);
        if (!description || group.submesh >= description->submeshes.size() ||
            !(*material)->gpuIndex(group.material_table_source))
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        auto value = std::unique_ptr<RasterMeshDraws>(new RasterMeshDraws(session, frames, registry, group));
        value->update_.resource = configuration.members;
        value->capacity_ = configuration.capacity;
        const CompiledResource section_plan{
            "sections",
            GraphBufferDesc{description->submeshes.size() * sizeof(MeshGpuSection)},
            EGraphOrigin::IMPORTED
        };
        StorageSubscription unused;
        auto sections = (*mesh)->resolve(
            section_plan,
            MeshBinding{group.mesh, EMeshBuffer::SECTIONS, sizeof(MeshGpuSection)},
            unused
        );
        if (!sections)
        {
            return cxx::unexpected(sections.error());
        }
        value->sections_ = *sections;
        (*instances)
            ->forEach([&](MeshInstanceId id, const RasterMeshInstanceStorage::Selection& input) noexcept
                      { value->update(id, &input); });
        value->observer_.receiver = value.get();
        value->observer_.changed =
            [](void* pointer, MeshInstanceId id, const RasterMeshInstanceStorage::Selection* description) noexcept
        {
            auto& self = *static_cast<RasterMeshDraws*>(pointer);
            self.update(id, description);
        };
        (*instances)->observe(value->observer_);
        return FeatureBinding{
            {value.release(), [](void* pointer) noexcept { delete static_cast<RasterMeshDraws*>(pointer); }},
            [](void* pointer, const FeatureFrameInput& input, ResourceUploader& uploads) noexcept
            { return static_cast<RasterMeshDraws*>(pointer)->prepare(input.slot, uploads); }
        };
    }
} // namespace lux::render::detail
