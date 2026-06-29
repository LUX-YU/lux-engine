#include <lux/engine/render/resources/point_cloud/GpuOctreeNodeBuffer.hpp>
#include <lux/engine/render/transfer/TransferScheduler.hpp>
#include <vk_mem_alloc.h>
#include <lux/engine/render/resources/lifecycle/VRAMBudgetGuard.hpp>

#include <cassert>
#include <cstring>

namespace lux::render
{

// ============================================================================
//  init / shutdown
// ============================================================================

bool GpuOctreeNodeBuffer::init(VmaAllocator allocator, uint32_t max_nodes)
{
    if (isInitialized()) return true;
    if (!allocator || max_nodes == 0) return false;

    allocator_ = allocator;
    max_nodes_ = max_nodes;

    // Allocate header (node_count + padding = 16 bytes) followed by the node array.
    // Layout must match the shader SSBO block in pointcloud_culling.comp:
    //   { uint node_count; uint padding[3]; GpuOctreeNode nodes[]; }
    const VkDeviceSize buf_size =
        kHeaderSize + static_cast<VkDeviceSize>(max_nodes) * sizeof(GpuOctreeNode);

    VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size        = buf_size;
    bci.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT  // compute shader read
                    | VK_BUFFER_USAGE_TRANSFER_DST_BIT    // staging target
                    | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;   // expansion copy source
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    const VkResult res = vmaCreateBuffer(allocator_, &bci, &aci,
                                         &buffer_, &allocation_, nullptr);
    if (res != VK_SUCCESS)
    {
        buffer_     = VK_NULL_HANDLE;
        allocation_ = VK_NULL_HANDLE;
        return false;
    }

    free_indices_.reserve(64);
    chunk_to_index_.reserve(256);
    return true;
}

void GpuOctreeNodeBuffer::shutdown()
{
    if (!isInitialized()) return;

    vmaDestroyBuffer(allocator_, buffer_, allocation_);
    buffer_     = VK_NULL_HANDLE;
    allocation_ = VK_NULL_HANDLE;

    allocator_  = nullptr;
    max_nodes_  = 0;

    chunk_to_index_.clear();
    free_indices_.clear();
    pending_zeros_.clear();
    next_index_ = 0;
}

// ============================================================================
//  Slot management helpers
// ============================================================================

uint32_t GpuOctreeNodeBuffer::acquireIndex()
{
    uint32_t idx;
    if (!free_indices_.empty())
    {
        idx = free_indices_.back();
        free_indices_.pop_back();
    }
    else
    {
        if (next_index_ >= max_nodes_) return kInvalidIndex;
        idx = next_index_++;
    }
    // A slot freed earlier THIS frame was queued for GPU zeroing (pending_zeros_);
    // if we now reuse it for a new node, the deferred zero-fill would overwrite
    // the node data upsertNode is about to stage (WAW + permanent invisibility).
    // Cancel the pending zero on reuse. Also covers removeAllNodes(), which queues
    // zeros and resets next_index_ to 0 so the next_index_++ path can collide too.
    std::erase(pending_zeros_, idx);
    return idx;
}

void GpuOctreeNodeBuffer::releaseIndex(uint32_t index)
{
    free_indices_.push_back(index);
}

// ============================================================================
//  Node operations
// ============================================================================

void GpuOctreeNodeBuffer::removeNode(uint32_t chunk_id)
{
    if (!chunk_to_index_.contains(chunk_id)) return;
    const uint32_t index = chunk_to_index_.at(chunk_id);

    // Queue the slot for GPU zeroing so the culling shader sees stream_state=0
    // and skips it.  Without this, freed slots retain stream_state=RESIDENT and
    // the compute shader would emit stale draw commands for dangling data.
    pending_zeros_.push_back(index);

    releaseIndex(index);
    (void)chunk_to_index_.erase(chunk_id);
}

void GpuOctreeNodeBuffer::removeAllNodes()
{
    const auto& keys = chunk_to_index_.keys();
    const auto& vals = chunk_to_index_.values();
    const std::size_t count = keys.size();
    for (std::size_t i = 0; i < count; ++i)
        pending_zeros_.push_back(vals[i]);
    chunk_to_index_.clear();
    free_indices_.clear();
    next_index_ = 0;
}

uint32_t GpuOctreeNodeBuffer::getIndex(uint32_t chunk_id) const noexcept
{
    if (!chunk_to_index_.contains(chunk_id)) return kInvalidIndex;
    return chunk_to_index_.at(chunk_id);
}

// ============================================================================
//  TransferScheduler-aware overloads
// ============================================================================

uint32_t GpuOctreeNodeBuffer::upsertNode(uint32_t chunk_id,
                                          const GpuOctreeNode& node,
                                          TransferScheduler& scheduler)
{
    assert(isInitialized());

    uint32_t index = kInvalidIndex;
    if (chunk_to_index_.contains(chunk_id))
    {
        index = chunk_to_index_.at(chunk_id);
    }
    else
    {
        index = acquireIndex();
        if (index == kInvalidIndex) return kInvalidIndex;
        chunk_to_index_.insert(chunk_id, index);
    }

    const VkDeviceSize byte_offset =
        kHeaderSize + static_cast<VkDeviceSize>(index) * sizeof(GpuOctreeNode);
    const VkDeviceSize byte_size = sizeof(GpuOctreeNode);

    StagingAlloc stg = scheduler.allocateStaging(byte_size);
    if (stg.mapped)
    {
        std::memcpy(stg.mapped, &node, byte_size);
        scheduler.submitBufferCopy({
            .src        = stg.buffer,
            .src_offset = stg.srcOffset,
            .dst        = buffer_,
            .dst_offset = byte_offset,
            .size       = byte_size,
            .domain     = EBufferDomain::Storage_CS,
            .priority   = 0,
        });
    }

    return index;
}

void GpuOctreeNodeBuffer::flushNodeCount(TransferScheduler& scheduler)
{
    if (!isInitialized()) return;

    const uint32_t new_count = next_index_;
    StagingAlloc hdr = scheduler.allocateStaging(sizeof(uint32_t));
    if (hdr.mapped)
    {
        std::memcpy(hdr.mapped, &new_count, sizeof(uint32_t));
        scheduler.submitBufferCopy({
            .src        = hdr.buffer,
            .src_offset = hdr.srcOffset,
            .dst        = buffer_,
            .dst_offset = 0,
            .size       = sizeof(uint32_t),
            .domain     = EBufferDomain::Storage_CS,
            .priority   = 0,
        });
    }
}

void GpuOctreeNodeBuffer::flushRemovedNodes(TransferScheduler& scheduler)
{
    if (pending_zeros_.empty() || !isInitialized()) return;

    static constexpr GpuOctreeNode kZero{};
    for (const uint32_t index : pending_zeros_)
    {
        const VkDeviceSize byte_offset =
            kHeaderSize + static_cast<VkDeviceSize>(index) * sizeof(GpuOctreeNode);

        StagingAlloc stg = scheduler.allocateStaging(sizeof(GpuOctreeNode));
        if (stg.mapped)
        {
            std::memcpy(stg.mapped, &kZero, sizeof(GpuOctreeNode));
            scheduler.submitBufferCopy({
                .src        = stg.buffer,
                .src_offset = stg.srcOffset,
                .dst        = buffer_,
                .dst_offset = byte_offset,
                .size       = sizeof(GpuOctreeNode),
                .domain     = EBufferDomain::Storage_CS,
                .priority   = 0,
            });
        }
    }
    pending_zeros_.clear();
}

} // namespace lux::render
