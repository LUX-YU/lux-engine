#pragma once

#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/device/ResourceUploader.hpp>
#include <lux/engine/render/resource/StorageMeta.hpp>
#include <array>
#include <bitset>

namespace sdk_particles
{
    using namespace lux::render;

    // Matches the Shader's StructuredBuffer element; a row index is stable for the lifetime of the resource.
    struct alignas(16) ParticleGpu final
    {
        std::array<float, 4> position;
        std::array<float, 4> color;
    };
    static_assert(sizeof(ParticleGpu) == 32 && offsetof(ParticleGpu, color) == 16);
    static_assert(std::is_standard_layout_v<ParticleGpu> && std::is_trivially_copyable_v<ParticleGpu>);

    struct ParticleUpdate final
    {
        std::uint32_t row;
        ParticleGpu value;
    };

    struct ParticleResource final
    {
        using Config = void;
        using PerFrameData = std::tuple<ParticleUpdate>;
        static constexpr std::size_t row_count = 2;
        static constexpr GraphBufferRange records{0, row_count * sizeof(ParticleGpu)};
        [[nodiscard]] static ResourceProjection projection() noexcept
        {
            return {GraphBufferDesc{records.size}, {records}, EGraphUse::SHADER_BUFFER_READ, EGraphUse::STORAGE_READ};
        }

        static inline const ResourceDefinition descriptor{
            ResourceNameView{"example.particles"}, 0, 0, {}, "Particle records indexed by the shader.",
            EResourceDomain::VIEW, resourceFrameTypes(static_cast<PerFrameData*>(nullptr))
        };
    };

    class ParticleStorage final
    {
        struct Replica final
        {
            nri::Buffer* buffer = nullptr;
            std::bitset<ParticleResource::row_count> dirty, staged;
        };

    public:
        using Descriptor = ParticleResource;

        ParticleStorage(detail::DeviceSession& device, std::uint32_t frames) noexcept
            : device_(device), replicas_(frames)
        {
        }

        ParticleStorage(const ParticleStorage&) = delete;
        ParticleStorage& operator=(const ParticleStorage&) = delete;

        ~ParticleStorage() noexcept
        {
            for (const auto& replica : replicas_)
            {
                if (replica.buffer)
                    device_.deferRelease([buffer = replica.buffer, destroy = device_.nriCore().DestroyBuffer]() noexcept
                    {
                        destroy(buffer);
                    });
            }
        }

        static RenderResult<std::unique_ptr<ParticleStorage>>
        create(ResourceCreateContext& context, ResourceInstanceId) noexcept
        {
            if (!context.uploads) return lux::cxx::unexpected(RenderError{kDeviceUnsupported});
            auto owner = std::make_unique<ParticleStorage>(context.device, context.frame_count);
            const nri::BufferDesc desc{
                Descriptor::records.size, sizeof(ParticleGpu), nri::BufferUsageBits::SHADER_RESOURCE
            };
            for (auto& replica : owner->replicas_)
            {
                const auto status = context.device.nriCore().CreatePlacedBuffer(
                    context.device.device(), NriDeviceHeap, desc, replica.buffer
                );
                if (status != nri::Result::SUCCESS)
                    return lux::cxx::unexpected(RenderError{kDeviceFailure, {static_cast<std::uint32_t>(status)}});
                replica.dirty.set();
            }
            return owner;
        }

        RenderResult<void> update(std::span<const ParticleUpdate> updates) noexcept
        {
            if (updates.empty()) return {};
            if (revision_ == UINT64_MAX) return lux::cxx::unexpected(RenderError{kStorageCapacity});
            for (const auto& update : updates)
                if (update.row >= rows_.size()) return lux::cxx::unexpected(RenderError{kStorageIdentity});
            for (const auto& update : updates)
            {
                rows_[update.row] = update.value;
                for (auto& replica : replicas_) replica.dirty.set(update.row);
            }
            ++revision_;
            return {};
        }

        static ResourceRegistration factory() noexcept
        {
            static const detail::StorageBindingOps binding{
                .prepareFrame = [](void* value, detail::ReplicaFrame frame, detail::ResourceUploader& uploader) noexcept
                {
                    return static_cast<ParticleStorage*>(value)->recordUploads(frame.slot, uploader);
                },
                .preparationRevision = [](const void* value) noexcept
                {
                    return &static_cast<const ParticleStorage*>(value)->revision_;
                },
                .describeGraphImport = [](const void*, std::uint32_t projection) noexcept
                    -> RenderResult<ResourceProjection>
                {
                    if (projection != 0) return lux::cxx::unexpected(RenderError{kStorageMismatch});
                    return Descriptor::projection();
                },
                .resolveGraphImport = [](
                    void* value, std::uint32_t projection, const VGraphResourceDesc& shape,
                    std::span<detail::ResourceInput> outputs
                ) noexcept -> RenderResult<void>
                {
                    const auto& replicas = static_cast<ParticleStorage*>(value)->replicas_;
                    const auto* buffer = std::get_if<GraphBufferDesc>(&shape);
                    const bool valid_shape = buffer && buffer->size == Descriptor::records.size;
                    if (projection != 0 || !valid_shape || outputs.size() != replicas.size())
                        return lux::cxx::unexpected(RenderError{kStorageMismatch});
                    for (std::size_t i = 0; i < replicas.size(); ++i)
                        outputs[i] = {replicas[i].buffer, 0, Descriptor::records.size};
                    return {};
                },
                .settleFrame = [](void* value, const detail::ReplicaSettlement& receipt) noexcept
                {
                    auto& replica = static_cast<ParticleStorage*>(value)->replicas_[receipt.frame.slot];
                    if (receipt.error.type || !receipt.accepted_copy.serial) replica.dirty |= replica.staged;
                    replica.staged.reset();
                }
            };
            return makeResourceRegistration<Descriptor, ParticleStorage, create, &ParticleStorage::update>(&binding);
        }

    private:
        RenderResult<bool> recordUploads(std::uint32_t slot, detail::ResourceUploader& uploader) noexcept
        {
            auto& replica = replicas_[slot];
            for (std::size_t row = 0; row < rows_.size(); ++row)
            {
                if (!replica.dirty.test(row)) continue;
                const auto uploaded = uploader.stageBuffer({
                    replica.buffer, row * sizeof(ParticleGpu), std::as_bytes(std::span{&rows_[row], 1})
                });
                if (!uploaded) return lux::cxx::unexpected(uploaded.error());
                // Staging owns these bytes. A later update can independently dirty this row again.
                replica.staged.set(row);
                replica.dirty.reset(row);
            }
            return true;
        }

        detail::DeviceSession& device_;
        std::vector<Replica> replicas_;
        std::array<ParticleGpu, ParticleResource::row_count> rows_{};
        std::uint64_t revision_ = 1;
    };
}
