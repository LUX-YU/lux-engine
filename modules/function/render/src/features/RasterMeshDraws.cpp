#include <bit>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/features/RasterMeshDraws.hpp>
#include <lux/engine/render/graph/CompiledGraph.hpp>
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
        if (input.mesh != selection_.mesh || input.submesh != selection_.submesh ||
            input.material.storage != selection_.material.storage)
        {
            return false;
        }
        auto material = resources_.borrow<RasterMaterialTableStorage>(selection_.material.storage);
        return material && (*material)->sameTable(input.material, selection_.material);
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
    }

    RenderResult<FeatureFrameWork::Preparation> RasterMeshDraws::prepare(
        std::uint32_t slot,
        ResourceUploader& uploads
    ) noexcept
    {
        if (!available_)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        auto& slice = gpu_[slot];
        slice.pending_revision = 0;
        const auto prepared = [&](FeatureFrameWork::EPreparation state) noexcept
        {
            FeatureFrameWork::Preparation result{state};
            if (std::exchange(slice.binding_changed, false))
            {
                result.changed_resource =
                    StorageResource{slice.buffer, 0, session_.nriCore().GetBufferDesc(*slice.buffer).size};
            }
            return result;
        };
        if (slice.uploaded_revision == revision_)
        {
            return prepared(FeatureFrameWork::EPreparation::READY);
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
                frames_.afterSubmit(
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
                return prepared(FeatureFrameWork::EPreparation::WAITING);
            }
            slice.pending_bytes += *copied;
        }
        slice.pending_revision = revision_;
        return prepared(FeatureFrameWork::EPreparation::RECORD);
    }

    void RasterMeshDraws::record(nri::CommandBuffer& command, std::uint32_t slot) noexcept
    {
        auto& slice = gpu_[slot];
        if (!slice.pending_revision)
        {
            return;
        }
        const auto& nri_core = session_.nriCore();
        nri::BufferBarrierDesc barrier{
            slice.buffer,
            {nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY},
            {nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY}
        };
        const nri::BarrierDesc dependency{nullptr, 0, &barrier, 1};
        nri_core.CmdBarrier(command, dependency);
        const auto section = sections_.offset + std::uint64_t(selection_.submesh) * sizeof(MeshGpuSection);
        nri_core.CmdCopyBuffer(command, *slice.buffer, 0, *sections_.buffer, section, 4);
        nri_core.CmdCopyBuffer(command, *slice.buffer, 8, *sections_.buffer, section + 4, 4);
        nri_core.CmdCopyBuffer(command, *slice.buffer, 12, *sections_.buffer, section + 8, 4);
        barrier.after = {
            nri::AccessBits::SHADER_RESOURCE_STORAGE | nri::AccessBits::ARGUMENT_BUFFER,
            nri::StageBits::ALL
        };
        nri_core.CmdBarrier(command, dependency);
    }

    RenderResult<FeatureFrameWork> RasterMeshDraws::create(
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
        auto material = registry.borrow<RasterMaterialTableStorage>(group.material.storage);
        if (!instances || !mesh || !material)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        const auto* description = (*mesh)->find(group.mesh);
        if (!description || group.submesh >= description->submeshes.size() || !(*material)->gpuIndex(group.material))
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        auto value = std::unique_ptr<RasterMeshDraws>(new RasterMeshDraws(session, frames, registry, group));
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
        return FeatureFrameWork{
            {value.release(), [](void* pointer) noexcept { delete static_cast<RasterMeshDraws*>(pointer); }},
            [](void* pointer, std::uint32_t slot, ResourceUploader& uploads) noexcept
            { return static_cast<RasterMeshDraws*>(pointer)->prepare(slot, uploads); },
            [](void* pointer, nri::CommandBuffer& command, std::uint32_t slot) noexcept
            { static_cast<RasterMeshDraws*>(pointer)->record(command, slot); }
        };
    }
} // namespace lux::render::detail
