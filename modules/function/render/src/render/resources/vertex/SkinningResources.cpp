/**
 * @file SkinningResources.cpp
 */

#include <lux/engine/render/resources/vertex/SkinningResources.hpp>

#include <cstring>
#include <iostream>

#include <lux/engine/render/core/VulkanContext.hpp>          // DeviceContext
#include <lux/engine/render/resources/memory/GPUBuffer.hpp>         // createGpuBufferVmaBuffer
#include <lux/engine/render/resources/vertex/VertexPoolRegistry.hpp>

namespace lux::render
{
    SkinningResources::SkinningResources() = default;

    SkinningResources::~SkinningResources()
    {
        if (initialized_) shutdown();
    }

    bool SkinningResources::init(const InitInfo& info)
    {
        if (initialized_) return true;
        if (!info.device_context || !info.vertex_pool_registry) return false;
        if (info.vertex_stride == 0 || info.max_bones == 0)     return false;
        if (info.max_dispatches == 0)                            return false;
        if (info.layout_id == kInvalidVertexLayoutId)           return false;

        device_ctx_           = info.device_context;
        vertex_pool_registry_ = info.vertex_pool_registry;
        max_bones_            = info.max_bones;
        max_dispatches_       = info.max_dispatches;

        // Bone palette: CPU-writable persistent-mapped SSBO, ring-buffered per
        // frame-in-flight (filled per frame by the upload command; read by the
        // skinning compute pass). See header for the ring-slot rationale.
        const VkDeviceSize palette_bytes =
            VkDeviceSize(max_bones_) * sizeof(BoneMatrixGpu);

        auto destroyPalettes = [this]() noexcept {
            for (std::uint32_t i = 0; i < kMaxFramesInFlight; ++i)
                if (bone_palette_buffers_[i] != VK_NULL_HANDLE)
                    vmaDestroyBuffer(device_ctx_->vmaAllocator(),
                                     bone_palette_buffers_[i], bone_palette_allocs_[i]);
            bone_palette_buffers_.fill(VK_NULL_HANDLE);
            bone_palette_allocs_.fill(nullptr);
            bone_palette_mapped_.fill(nullptr);
        };

        for (std::uint32_t i = 0; i < kMaxFramesInFlight; ++i)
        {
            if (!createGpuBufferVmaBuffer(device_ctx_->vmaAllocator(),
                                          palette_bytes,
                                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                          /*cpu_writable=*/true,
                                          &bone_palette_buffers_[i],
                                          &bone_palette_allocs_[i],
                                          &bone_palette_mapped_[i]))
            {
                destroyPalettes();
                return false;
            }
        }

        // Skinned-vertex output pool + register it for a bindless pool id.
        TransientVertexSource::InitInfo tvs{};
        tvs.device_context = device_ctx_;
        tvs.capacity_bytes = info.output_pool_bytes;
        tvs.layout_id      = info.layout_id;
        tvs.vertex_stride  = info.vertex_stride;
        if (!output_pool_.init(tvs)) {
            destroyPalettes();
            return false;
        }

        if (vertex_pool_registry_->registerSource(output_pool_) == ~0u) {
            output_pool_.shutdown();
            destroyPalettes();
            return false;
        }
        output_registered_ = true;

        // Input vertex pool registration lives in the per-scene MeshInputPool.
        // The caller passes the input pool id as a queueDispatch argument and
        // the compute kernel reads it from the dispatch-params SSBO entry.

        // Dispatch-params SSBO ring — one entry per skinned instance
        // per frame. CPU writes `SkinDispatchParams[]` here each frame; the
        // skin compute reads it and binary-searches by gl_WorkGroupID.x.
        const VkDeviceSize dispatch_params_bytes =
            VkDeviceSize(max_dispatches_) * sizeof(SkinDispatchParams);

        auto destroyDispatchRings = [this]() noexcept {
            for (std::uint32_t i = 0; i < kMaxFramesInFlight; ++i)
                if (dispatch_params_buffers_[i] != VK_NULL_HANDLE)
                    vmaDestroyBuffer(device_ctx_->vmaAllocator(),
                                     dispatch_params_buffers_[i], dispatch_params_allocs_[i]);
            dispatch_params_buffers_.fill(VK_NULL_HANDLE);
            dispatch_params_allocs_.fill(nullptr);
            dispatch_params_mapped_.fill(nullptr);
        };

        for (std::uint32_t i = 0; i < kMaxFramesInFlight; ++i)
        {
            if (!createGpuBufferVmaBuffer(device_ctx_->vmaAllocator(),
                                          dispatch_params_bytes,
                                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                          /*cpu_writable=*/true,
                                          &dispatch_params_buffers_[i],
                                          &dispatch_params_allocs_[i],
                                          &dispatch_params_mapped_[i]))
            {
                destroyDispatchRings();
                if (output_registered_) {
                    vertex_pool_registry_->unregisterSource(output_pool_.bindlessPoolId());
                    output_registered_ = false;
                }
                output_pool_.shutdown();
                destroyPalettes();
                return false;
            }
        }

        palette_cursors_.fill(0);
        current_fi_ = 0;
        dispatches_.clear();
        initialized_ = true;
        return true;
    }

    void SkinningResources::shutdown()
    {
        if (!initialized_) return;

        if (output_registered_ && vertex_pool_registry_) {
            vertex_pool_registry_->unregisterSource(output_pool_.bindlessPoolId());
            output_registered_ = false;
        }
        output_pool_.shutdown();

        for (std::uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
            if (bone_palette_buffers_[i] != VK_NULL_HANDLE)
                vmaDestroyBuffer(device_ctx_->vmaAllocator(),
                                 bone_palette_buffers_[i], bone_palette_allocs_[i]);
            if (dispatch_params_buffers_[i] != VK_NULL_HANDLE)
                vmaDestroyBuffer(device_ctx_->vmaAllocator(),
                                 dispatch_params_buffers_[i], dispatch_params_allocs_[i]);
        }
        bone_palette_buffers_.fill(VK_NULL_HANDLE);
        bone_palette_allocs_.fill(nullptr);
        bone_palette_mapped_.fill(nullptr);
        dispatch_params_buffers_.fill(VK_NULL_HANDLE);
        dispatch_params_allocs_.fill(nullptr);
        dispatch_params_mapped_.fill(nullptr);

        dispatches_.clear();
        palette_cursors_.fill(0);
        current_fi_           = 0;
        max_bones_            = 0;
        max_dispatches_       = 0;
        vertex_pool_registry_ = nullptr;
        device_ctx_           = nullptr;
        initialized_          = false;
    }

    void SkinningResources::beginFrame() noexcept
    {
        // current_fi_ was set by beginFrameIfNew (serial % kMaxFramesInFlight)
        // before this call; reset only the slot we're about to write.
        palette_cursors_[current_fi_] = 0;
        dispatches_.clear();
        output_pool_.beginFrame();
    }

    std::uint32_t SkinningResources::uploadBonePalette(const BoneMatrixGpu* bones,
                                                       std::uint32_t        bone_count)
    {
        if (!initialized_ || bones == nullptr || bone_count == 0) return ~0u;
        const std::uint32_t fi = current_fi_;
        if (bone_count > max_bones_ - palette_cursors_[fi]) return ~0u;  // full this frame

        const std::uint32_t base = palette_cursors_[fi];
        auto* dst = static_cast<BoneMatrixGpu*>(bone_palette_mapped_[fi]) + base;
        std::memcpy(dst, bones, bone_count * sizeof(BoneMatrixGpu));
        palette_cursors_[fi] += bone_count;
        return base;
    }

    VertexSourceHandle SkinningResources::queueDispatch(std::uint32_t in_pool_id,
                                                        std::uint32_t in_base,
                                                        std::uint32_t vertex_count,
                                                        std::uint32_t palette_base,
                                                        std::uint32_t bone_count)
    {
        if (!initialized_ || vertex_count == 0) return kInvalidVertexSourceHandle;

        const VertexSourceHandle out = output_pool_.allocate(vertex_count);
        if (!out.valid()) return kInvalidVertexSourceHandle;  // output pool exhausted

        dispatches_.push_back(Dispatch{
            in_pool_id, in_base, out.vertex_base, vertex_count, palette_base, bone_count});
        return out;
    }

    std::uint32_t SkinningResources::uploadDispatches() noexcept
    {
        if (!initialized_ || dispatches_.empty()) return 0u;

        const std::uint32_t fi = current_fi_;
        const std::uint32_t dispatch_count =
            static_cast<std::uint32_t>(dispatches_.size());
        if (dispatch_count > max_dispatches_) {
            // Loud-fail: dispatch-params SSBO too small for this frame.
            // The render thread MUST cap the dispatch count to fit; otherwise
            // we'd write past the buffer. Skip the batch and warn-once.
            static std::uint32_t warn_n = 0;
            if ((++warn_n & (warn_n - 1)) == 0)
                std::cerr << "[Skinning] dispatch-params SSBO capacity exceeded "
                          << "(have=" << max_dispatches_ << " queued="
                          << dispatch_count << "); batch skipped (x"
                          << warn_n << ")\n";
            return 0u;
        }

        auto* dst = static_cast<SkinDispatchParams*>(dispatch_params_mapped_[fi]);
        if (dst == nullptr) return 0u;

        // Running prefix sum of workgroups per dispatch, gl_WorkGroupID.x base
        // for entry `i`. Total workgroups for this frame's batched dispatch:
        //   sum(ceil(vc[j] / kSkinWorkgroupSize), j < dispatch_count)
        std::uint32_t wg_acc = 0u;
        for (std::uint32_t i = 0; i < dispatch_count; ++i)
        {
            const Dispatch& d = dispatches_[i];
            dst[i] = SkinDispatchParams{
                /*workgroup_start*/ wg_acc,
                /*vertex_count*/    d.vertex_count,
                /*in_base*/         d.in_base,
                /*out_base*/        d.out_base,
                /*palette_base*/    d.palette_base,
                /*in_pool_id*/      d.in_pool_id,
            };
            wg_acc += (d.vertex_count + kSkinWorkgroupSize - 1u) / kSkinWorkgroupSize;
        }
        return wg_acc;
    }

} // namespace lux::render
