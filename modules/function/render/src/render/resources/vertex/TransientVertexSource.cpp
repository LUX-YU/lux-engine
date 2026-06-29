/**
 * @file TransientVertexSource.cpp
 */

#include <lux/engine/render/resources/vertex/TransientVertexSource.hpp>

#include <lux/engine/render/core/VulkanContext.hpp>  // DeviceContext
#include <lux/engine/render/resources/memory/GPUBuffer.hpp> // createGpuBufferVmaBuffer

namespace lux::render
{
    TransientVertexSource::~TransientVertexSource()
    {
        if (initialized_) shutdown();
    }

    bool TransientVertexSource::init(const InitInfo& info)
    {
        if (initialized_) return true;
        if (info.device_context == nullptr) return false;
        if (info.vertex_stride == 0)        return false;
        if (info.capacity_bytes == 0)       return false;
        if (info.layout_id == kInvalidVertexLayoutId) return false;

        device_ctx_     = info.device_context;
        capacity_bytes_ = info.capacity_bytes;
        layout_id_      = info.layout_id;
        vertex_stride_  = info.vertex_stride;

        // SkinningFeature (R5) writes via compute → mesh shaders read.
        // STORAGE_BUFFER_BIT is the only usage we need here; no transfer
        // path involved (the data is always GPU-generated, never copied
        // from CPU).
        void* dummy_mapped = nullptr;
        const bool ok = createGpuBufferVmaBuffer(
            device_ctx_->vmaAllocator(),
            capacity_bytes_,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            /*cpu_writable=*/false,
            &buffer_,
            &alloc_,
            &dummy_mapped);
        if (!ok) {
            buffer_ = VK_NULL_HANDLE;
            alloc_  = nullptr;
            return false;
        }

        total_vertices_ = static_cast<std::uint32_t>(capacity_bytes_ / vertex_stride_);
        next_vertex_    = 0;
        initialized_    = true;
        return true;
    }

    void TransientVertexSource::shutdown()
    {
        if (!initialized_) return;

        if (buffer_ != VK_NULL_HANDLE) {
            vmaDestroyBuffer(device_ctx_->vmaAllocator(), buffer_, alloc_);
            buffer_ = VK_NULL_HANDLE;
            alloc_  = nullptr;
        }

        device_ctx_     = nullptr;
        capacity_bytes_ = 0;
        layout_id_      = kInvalidVertexLayoutId;
        vertex_stride_  = 0;
        pool_id_        = ~0u;
        next_vertex_    = 0;
        total_vertices_ = 0;
        initialized_    = false;
    }

    void TransientVertexSource::beginFrame() noexcept
    {
        // Arena reset — all previously-allocated ranges become invalid for
        // the new frame. Producers must re-allocate every frame.
        next_vertex_ = 0;
    }

    VertexSourceHandle TransientVertexSource::allocate(std::uint32_t vertex_count)
    {
        if (!initialized_ || vertex_count == 0) {
            return kInvalidVertexSourceHandle;
        }

        // Overflow check first so we can't wrap.
        if (vertex_count > total_vertices_ - next_vertex_) {
            return kInvalidVertexSourceHandle;
        }

        VertexSourceHandle h{};
        h.pool_id      = pool_id_;
        h.vertex_base  = next_vertex_;
        h.vertex_count = vertex_count;

        next_vertex_ += vertex_count;
        return h;
    }

} // namespace lux::render
