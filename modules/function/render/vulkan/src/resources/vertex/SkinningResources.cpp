/**
 * @file SkinningResources.cpp
 */

#include <lux/engine/render/resources/vertex/SkinningResources.hpp>

#include <cstring>

#include <lux/engine/render/core/RenderErrorSink.hpp>

#include <lux/engine/render/gpu/VulkanContext.hpp>    // DeviceContext
#include <lux/engine/render/gpu/memory/GPUBuffer.hpp> // createGpuBufferVmaBuffer
#include <lux/engine/render/resources/vertex/VertexPoolRegistry.hpp>

namespace lux::render
{
    Expected<SkinningResources::MappedBuffer>
    SkinningResources::createMappedBuffer(DeviceContext& device, VkDeviceSize size) noexcept
    {
        VkBuffer buffer{};
        VmaAllocation allocation{};
        void* mapped{};
        const auto allocator = device.vmaAllocator();
        const auto status = createGpuBufferVmaBuffer(
            allocator,
            size,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            true,
            &buffer,
            &allocation,
            &mapped
        );
        if (status != VK_SUCCESS)
        {
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(status));
        }
        MappedBuffer candidate{VmaBuffer::adopt({allocator, buffer, allocation}), mapped};
        if (!mapped)
        {
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(VK_ERROR_MEMORY_MAP_FAILED));
        }
        return candidate;
    }

    SkinningResources::CreateResult SkinningResources::create(const CreateInfo& info) noexcept
    {
        const bool is_missing_dependency = !info.device_context || !info.vertex_pool_registry;
        const bool is_invalid_capacity = info.max_bones == 0 || info.max_dispatches == 0;
        const bool is_invalid_configuration = is_missing_dependency || is_invalid_capacity;
        if (is_invalid_configuration)
        {
            return renderFailure<err::memory::InvalidSkinningConfiguration>();
        }
        auto output = TransientVertexSource::create(
            {info.device_context, info.output_pool_bytes, info.layout_id, info.vertex_stride}
        );
        if (!output)
        {
            return lux::cxx::unexpected(output.error());
        }
        BufferRing palettes;
        BufferRing dispatch_params;
        for (auto& palette : palettes)
        {
            auto candidate =
                createMappedBuffer(*info.device_context, VkDeviceSize(info.max_bones) * sizeof(BoneMatrixGpu));
            if (!candidate)
            {
                return lux::cxx::unexpected(candidate.error());
            }
            palette = std::move(*candidate);
        }
        for (auto& parameters : dispatch_params)
        {
            auto candidate = createMappedBuffer(
                *info.device_context,
                VkDeviceSize(info.max_dispatches) * sizeof(SkinDispatchParams)
            );
            if (!candidate)
            {
                return lux::cxx::unexpected(candidate.error());
            }
            parameters = std::move(*candidate);
        }
        // Only complete backing may become visible through the vertex source table.
        // The unique owner keeps the source address stable through semantic adoption.
        if (info.vertex_pool_registry->registerSource(**output) == ~0u)
        {
            return renderFailure<err::frame::VertexPoolRegistryFull>(kVertexPoolMaxCount);
        }
        return std::unique_ptr<SkinningResources>(
            new SkinningResources(info, std::move(palettes), std::move(dispatch_params), std::move(*output))
        );
    }

    SkinningResources::SkinningResources(
        const CreateInfo& info,
        BufferRing palettes,
        BufferRing dispatch_params,
        std::unique_ptr<TransientVertexSource> output
    ) noexcept
        : vertex_pool_registry_(*info.vertex_pool_registry), bone_palettes_(std::move(palettes)),
          max_bones_(info.max_bones), error_sink_(info.error_sink), dispatch_params_(std::move(dispatch_params)),
          max_dispatches_(info.max_dispatches), output_pool_(std::move(output))
    {
    }

    SkinningResources::~SkinningResources() noexcept
    {
        // Preserve the original scene safe point and revoke the source before member buffers disappear.
        vertex_pool_registry_.unregisterSource(output_pool_->bindlessPoolId());
    }

    void SkinningResources::beginFrame() noexcept
    {
        // current_fi_ was set by beginFrameIfNew (serial % kMaxFramesInFlight)
        // before this call; reset only the slot we're about to write.
        palette_cursors_[current_fi_] = 0;
        dispatches_.clear();
        output_pool_->beginFrame();
    }

    std::uint32_t SkinningResources::uploadBonePalette(const BoneMatrixGpu* bones, std::uint32_t bone_count)
    {
        if (bones == nullptr || bone_count == 0)
        {
            return ~0u;
        }
        const std::uint32_t fi = current_fi_;
        if (bone_count > max_bones_ - palette_cursors_[fi])
            return ~0u; // full this frame

        const std::uint32_t base = palette_cursors_[fi];
        auto* dst = static_cast<BoneMatrixGpu*>(bone_palettes_[fi].data) + base;
        std::memcpy(dst, bones, bone_count * sizeof(BoneMatrixGpu));
        palette_cursors_[fi] += bone_count;
        return base;
    }

    VertexSourceHandle SkinningResources::queueDispatch(
        std::uint32_t in_pool_id,
        std::uint32_t in_base,
        std::uint32_t vertex_count,
        std::uint32_t palette_base,
        std::uint32_t bone_count
    )
    {
        if (vertex_count == 0)
        {
            return kInvalidVertexSourceHandle;
        }

        const VertexSourceHandle out = output_pool_->allocate(vertex_count);
        if (!out.valid())
            return kInvalidVertexSourceHandle; // output pool exhausted

        dispatches_.push_back(Dispatch{in_pool_id, in_base, out.vertex_base, vertex_count, palette_base, bone_count});
        return out;
    }

    std::uint32_t SkinningResources::uploadDispatches() noexcept
    {
        if (dispatches_.empty())
        {
            return 0u;
        }

        const std::uint32_t fi = current_fi_;
        const std::uint32_t dispatch_count = static_cast<std::uint32_t>(dispatches_.size());
        // dispatch 参数缓冲装不下本帧这一批 —— 继续写会越过缓冲边界,只能整批跳过。
        if (dispatch_count > max_dispatches_)
        {
            if (error_sink_ != nullptr)
                error_sink_->emit(
                    renderError<err::frame::SkinningDispatchParamsOverflow>(dispatch_count, max_dispatches_),
                    RenderErrorEvent::kNoScene,
                    last_frame_serial_
                );
            return 0u;
        }

        auto* dst = static_cast<SkinDispatchParams*>(dispatch_params_[fi].data);
        // Running prefix sum of workgroups per dispatch, gl_WorkGroupID.x base
        // for entry `i`. Total workgroups for this frame's batched dispatch:
        //   sum(ceil(vc[j] / kSkinWorkgroupSize), j < dispatch_count)
        std::uint32_t wg_acc = 0u;
        for (std::uint32_t i = 0; i < dispatch_count; ++i)
        {
            const Dispatch& d = dispatches_[i];
            dst[i] = SkinDispatchParams{
                /*workgroup_start*/ wg_acc,
                /*vertex_count*/ d.vertex_count,
                /*in_base*/ d.in_base,
                /*out_base*/ d.out_base,
                /*palette_base*/ d.palette_base,
                /*in_pool_id*/ d.in_pool_id,
            };
            wg_acc += (d.vertex_count + kSkinWorkgroupSize - 1u) / kSkinWorkgroupSize;
        }
        return wg_acc;
    }

} // namespace lux::render
