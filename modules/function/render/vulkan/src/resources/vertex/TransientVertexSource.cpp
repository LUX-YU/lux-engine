/**
 * @file TransientVertexSource.cpp
 */

#include <lux/engine/render/resources/vertex/TransientVertexSource.hpp>

#include <lux/engine/render/gpu/VulkanContext.hpp>    // DeviceContext
#include <lux/engine/render/gpu/memory/GPUBuffer.hpp> // createGpuBufferVmaBuffer

namespace lux::render
{
    TransientVertexSource::CreateResult TransientVertexSource::create(const CreateInfo& info) noexcept
    {
        const bool is_missing_device = info.device_context == nullptr || info.device_context->vmaAllocator() == nullptr;
        const bool is_invalid_stride = info.vertex_stride == 0;
        const bool is_invalid_layout = info.layout_id == kInvalidVertexLayoutId;
        const bool is_invalid_capacity = is_invalid_stride || info.capacity_bytes < info.vertex_stride ||
                                         info.capacity_bytes / info.vertex_stride > UINT32_MAX;
        const bool is_invalid_configuration = is_missing_device || is_invalid_layout || is_invalid_capacity;
        if (is_invalid_configuration)
        {
            return renderFailure<err::memory::InvalidTransientVertexConfiguration>();
        }
        VkBuffer buffer{};
        VmaAllocation allocation{};
        const auto allocator = info.device_context->vmaAllocator();
        const auto status = createGpuBufferVmaBuffer(
            allocator,
            info.capacity_bytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            false,
            &buffer,
            &allocation,
            nullptr
        );
        if (status != VK_SUCCESS)
        {
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(status));
        }
        auto owner = VmaBuffer::adopt({allocator, buffer, allocation});
        return std::unique_ptr<TransientVertexSource>(new TransientVertexSource(
            std::move(owner),
            info.layout_id,
            static_cast<std::uint32_t>(info.capacity_bytes / info.vertex_stride)
        ));
    }

    TransientVertexSource::TransientVertexSource(
        VmaBuffer buffer,
        VertexLayoutId layout,
        std::uint32_t total_vertices
    ) noexcept
        : buffer_(std::move(buffer)), layout_id_(layout), total_vertices_(total_vertices)
    {
    }

    void TransientVertexSource::beginFrame() noexcept
    {
        // Arena reset — all previously-allocated ranges become invalid for
        // the new frame. Producers must re-allocate every frame.
        next_vertex_ = 0;
    }

    VertexSourceHandle TransientVertexSource::allocate(std::uint32_t vertex_count)
    {
        if (vertex_count == 0)
        {
            return kInvalidVertexSourceHandle;
        }

        // Overflow check first so we can't wrap.
        if (vertex_count > total_vertices_ - next_vertex_)
        {
            return kInvalidVertexSourceHandle;
        }

        VertexSourceHandle h{};
        h.pool_id = pool_id_;
        h.vertex_base = next_vertex_;
        h.vertex_count = vertex_count;

        next_vertex_ += vertex_count;
        return h;
    }

} // namespace lux::render
