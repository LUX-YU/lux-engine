#include <lux/engine/render/transfer/TransferScheduler.hpp>
#include <vk_mem_alloc.h>

#include <algorithm>
#include <cstring>
#include <cassert>

namespace lux::render
{

// =====================================================================
//  Lifecycle
// =====================================================================

TransferScheduler::~TransferScheduler()
{
    shutdown();
}

TransferScheduler::TransferScheduler(TransferScheduler &&o) noexcept
    : allocator_{o.allocator_},
      frames_in_flight_{o.frames_in_flight_},
      ring_{std::move(o.ring_)},
      overflow_staging_{std::move(o.overflow_staging_)},
      deferred_staging_{std::move(o.deferred_staging_)},
      contributors_{std::move(o.contributors_)},
      buffer_copies_{std::move(o.buffer_copies_)},
      image_copies_{std::move(o.image_copies_)},
      qfot_acquires_{std::move(o.qfot_acquires_)},
      extra_post_barriers_{std::move(o.extra_post_barriers_)},
      pre_buffer_barriers_{std::move(o.pre_buffer_barriers_)},
      post_buffer_barriers_{std::move(o.post_buffer_barriers_)},
      pre_image_barriers_{std::move(o.pre_image_barriers_)},
      post_image_barriers_{std::move(o.post_image_barriers_)},
      last_barrier_count_{o.last_barrier_count_},
      last_copy_count_{o.last_copy_count_},
      initialized_{o.initialized_}
{
    o.allocator_   = nullptr;
    o.initialized_ = false;
}

TransferScheduler &TransferScheduler::operator=(TransferScheduler &&o) noexcept
{
    if (this != &o)
    {
        shutdown();
        allocator_            = o.allocator_;
        frames_in_flight_     = o.frames_in_flight_;
        ring_                 = std::move(o.ring_);
        overflow_staging_     = std::move(o.overflow_staging_);
        deferred_staging_     = std::move(o.deferred_staging_);
        contributors_         = std::move(o.contributors_);
        buffer_copies_        = std::move(o.buffer_copies_);
        image_copies_         = std::move(o.image_copies_);
        qfot_acquires_        = std::move(o.qfot_acquires_);
        extra_post_barriers_  = std::move(o.extra_post_barriers_);
        pre_buffer_barriers_  = std::move(o.pre_buffer_barriers_);
        post_buffer_barriers_ = std::move(o.post_buffer_barriers_);
        pre_image_barriers_   = std::move(o.pre_image_barriers_);
        post_image_barriers_  = std::move(o.post_image_barriers_);
        last_barrier_count_   = o.last_barrier_count_;
        last_copy_count_      = o.last_copy_count_;
        initialized_          = o.initialized_;
        o.allocator_          = nullptr;
        o.initialized_        = false;
    }
    return *this;
}

bool TransferScheduler::init(const Config &cfg)
{
    if (initialized_) return false;
    allocator_        = cfg.allocator;
    frames_in_flight_ = cfg.frames_in_flight;

    if (!ring_.init(cfg.allocator, cfg.ring_capacity, cfg.frames_in_flight))
        return false;

    deferred_staging_.resize(frames_in_flight_);
    initialized_ = true;
    return true;
}

void TransferScheduler::shutdown()
{
    if (!initialized_) return;

    // Flush all deferred staging buffers.
    for (auto &slot : deferred_staging_)
        slot.clear();
    overflow_staging_.clear();

    ring_ = {};
    initialized_ = false;
}

// =====================================================================
//  Staging allocation
// =====================================================================

StagingAlloc TransferScheduler::allocateStaging(VkDeviceSize bytes)
{
    assert(initialized_);

    // Fast path: ring sub-allocation.
    auto sub = ring_.suballocate(bytes);
    if (sub)
        return {sub.buffer, /*allocation=*/nullptr, sub.mapped, sub.offset};

    // Overflow: dedicated VMA staging buffer.
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size  = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
              | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VmaAllocationInfo info{};
    VkBuffer      buf   = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    if (vmaCreateBuffer(allocator_, &bci, &aci, &buf, &alloc, &info) != VK_SUCCESS)
        return {};

    overflow_staging_.emplace_back(allocator_, buf, alloc);
    return {buf, alloc, info.pMappedData, /*srcOffset=*/0};
}

// =====================================================================
//  Copy request submission
// =====================================================================

void TransferScheduler::submitBufferCopy(const BufferCopyRequest &req)
{
    buffer_copies_.push_back(req);
}

void TransferScheduler::submitBufferCopies(std::span<const BufferCopyRequest> reqs)
{
    buffer_copies_.insert(buffer_copies_.end(), reqs.begin(), reqs.end());
}

void TransferScheduler::submitImageCopy(const ImageCopyRequest &req)
{
    image_copies_.push_back(req);
}

void TransferScheduler::submitQFOTAcquire(const QFOTAcquireRequest &req)
{
    qfot_acquires_.push_back(req);
}

void TransferScheduler::submitExtraPostBarrier(const VkBufferMemoryBarrier2 &barrier)
{
    extra_post_barriers_.push_back(barrier);
}

// =====================================================================
//  Contributor management
// =====================================================================

void TransferScheduler::collectFromContributors()
{
    contributors_.dispatch(*this);
}

// =====================================================================
//  Frame execution
// =====================================================================

void TransferScheduler::executeAll(VkCommandBuffer cmd)
{
    collectFromContributors();

    if (hasWork())
    {
        beginTransfers(cmd);
        recordCopies(cmd);
        endTransfers(cmd);
    }

    // Post-transfer phase: mip generation, etc.
    contributors_.dispatchPostTransfer(cmd);
}

bool TransferScheduler::hasWork() const noexcept
{
    return !buffer_copies_.empty() || !image_copies_.empty()
        || !qfot_acquires_.empty() || !extra_post_barriers_.empty();
}

// ── Pre-copy barriers ───────────────────────────────────────────────

void TransferScheduler::buildPreBarriers()
{
    pre_buffer_barriers_.clear();
    pre_image_barriers_.clear();

    // ── Buffer pre-barriers: READ→TRANSFER_WRITE ────────────────────
    // Sort by dst buffer for deduplication.
    std::stable_sort(buffer_copies_.begin(), buffer_copies_.end(),
                     [](const BufferCopyRequest &a, const BufferCopyRequest &b)
                     {
                         if (a.dst != b.dst) return a.dst < b.dst;
                         return a.priority < b.priority;
                     });

    VkBuffer prev_buf = VK_NULL_HANDLE;
    EBufferDomain prev_domain{};
    for (const auto &c : buffer_copies_)
    {
        if (c.domain == EBufferDomain::TransferDst) continue; // New buffer, no reader yet.
        if (c.dst == prev_buf && c.domain == prev_domain) continue; // Already emitted.

        auto sa = domainToStageAccess(c.domain);

        VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        b.srcStageMask  = sa.stages;
        b.srcAccessMask = sa.access;
        b.dstStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.buffer = c.dst;
        b.offset = 0;
        b.size   = VK_WHOLE_SIZE;

        pre_buffer_barriers_.push_back(b);
        prev_buf    = c.dst;
        prev_domain = c.domain;
    }

    // ── Image pre-barriers: layout transition + READ→TRANSFER_WRITE ─
    for (const auto &c : image_copies_)
    {
        if (c.domain == EBufferDomain::TransferDst && c.old_layout == VK_IMAGE_LAYOUT_UNDEFINED)
            continue; // Brand-new image.

        auto sa = domainToStageAccess(c.domain);

        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask  = (c.old_layout == VK_IMAGE_LAYOUT_UNDEFINED)
                              ? VK_PIPELINE_STAGE_2_NONE
                              : sa.stages;
        b.srcAccessMask = (c.old_layout == VK_IMAGE_LAYOUT_UNDEFINED)
                              ? VK_ACCESS_2_NONE
                              : sa.access;
        b.dstStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.oldLayout     = c.old_layout;
        b.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image         = c.dst;
        b.subresourceRange = {
            c.subresource.aspectMask,
            c.subresource.mipLevel, 1,
            c.subresource.baseArrayLayer, c.subresource.layerCount,
        };

        pre_image_barriers_.push_back(b);
    }
}

void TransferScheduler::beginTransfers(VkCommandBuffer cmd)
{
    buildPreBarriers();

    if (pre_buffer_barriers_.empty() && pre_image_barriers_.empty())
        return;

    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.bufferMemoryBarrierCount = static_cast<uint32_t>(pre_buffer_barriers_.size());
    dep.pBufferMemoryBarriers    = pre_buffer_barriers_.data();
    dep.imageMemoryBarrierCount  = static_cast<uint32_t>(pre_image_barriers_.size());
    dep.pImageMemoryBarriers     = pre_image_barriers_.data();

    vkCmdPipelineBarrier2(cmd, &dep);
    ++last_barrier_count_;
}

// ── Copy recording ──────────────────────────────────────────────────

void TransferScheduler::recordCopies(VkCommandBuffer cmd)
{
    // Sort buffer copies by priority so grow copies (-1) come before data (0).
    std::stable_sort(buffer_copies_.begin(), buffer_copies_.end(),
                     [](const BufferCopyRequest &a, const BufferCopyRequest &b)
                     { return a.priority < b.priority; });

    // Detect if we have a mix of priorities (grow + data) that need a mid-barrier.
    bool needs_mid_barrier = false;
    if (buffer_copies_.size() >= 2)
    {
        auto min_p = buffer_copies_.front().priority;
        auto max_p = buffer_copies_.back().priority;
        needs_mid_barrier = (min_p < 0 && max_p >= 0);
    }

    if (needs_mid_barrier)
    {
        // Record low-priority (grow) copies first.
        for (const auto &c : buffer_copies_)
        {
            if (c.priority >= 0) break;
            VkBufferCopy2 region{VK_STRUCTURE_TYPE_BUFFER_COPY_2};
            region.srcOffset = c.src_offset;
            region.dstOffset = c.dst_offset;
            region.size      = c.size;

            VkCopyBufferInfo2 info{VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2};
            info.srcBuffer   = c.src;
            info.dstBuffer   = c.dst;
            info.regionCount = 1;
            info.pRegions    = &region;
            vkCmdCopyBuffer2(cmd, &info);
            ++last_copy_count_;
        }

        // Mid-barrier: WAW TRANSFER_WRITE → TRANSFER_WRITE.
        VkMemoryBarrier2 mem{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mem.srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        mem.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        mem.dstStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        mem.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;

        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers    = &mem;
        vkCmdPipelineBarrier2(cmd, &dep);
        ++last_barrier_count_;

        // Record normal-priority copies.
        for (const auto &c : buffer_copies_)
        {
            if (c.priority < 0) continue;
            VkBufferCopy2 region{VK_STRUCTURE_TYPE_BUFFER_COPY_2};
            region.srcOffset = c.src_offset;
            region.dstOffset = c.dst_offset;
            region.size      = c.size;

            VkCopyBufferInfo2 info{VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2};
            info.srcBuffer   = c.src;
            info.dstBuffer   = c.dst;
            info.regionCount = 1;
            info.pRegions    = &region;
            vkCmdCopyBuffer2(cmd, &info);
            ++last_copy_count_;
        }
    }
    else
    {
        // No grow copies — batch all buffer copies.
        for (const auto &c : buffer_copies_)
        {
            VkBufferCopy2 region{VK_STRUCTURE_TYPE_BUFFER_COPY_2};
            region.srcOffset = c.src_offset;
            region.dstOffset = c.dst_offset;
            region.size      = c.size;

            VkCopyBufferInfo2 info{VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2};
            info.srcBuffer   = c.src;
            info.dstBuffer   = c.dst;
            info.regionCount = 1;
            info.pRegions    = &region;
            vkCmdCopyBuffer2(cmd, &info);
            ++last_copy_count_;
        }
    }

    // ── Image copies ────────────────────────────────────────────────
    for (const auto &c : image_copies_)
    {
        VkBufferImageCopy2 region{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
        region.bufferOffset      = c.src_offset;
        region.bufferRowLength   = 0; // Tightly packed.
        region.bufferImageHeight = 0;
        region.imageSubresource  = c.subresource;
        region.imageOffset       = c.offset;
        region.imageExtent       = c.extent;

        VkCopyBufferToImageInfo2 info{VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2};
        info.srcBuffer        = c.src;
        info.dstImage         = c.dst;
        info.dstImageLayout   = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        info.regionCount      = 1;
        info.pRegions         = &region;
        vkCmdCopyBufferToImage2(cmd, &info);
        ++last_copy_count_;
    }
}

// ── Post-copy barriers ──────────────────────────────────────────────

void TransferScheduler::buildPostBarriers()
{
    post_buffer_barriers_.clear();
    post_image_barriers_.clear();

    // ── Buffer post-barriers: TRANSFER_WRITE→READ ───────────────────
    // Re-sort by dst for dedup (may have been reordered by priority sort).
    auto sorted = buffer_copies_;
    std::sort(sorted.begin(), sorted.end(),
              [](const BufferCopyRequest &a, const BufferCopyRequest &b)
              {
                  if (a.dst != b.dst) return a.dst < b.dst;
                  return static_cast<uint8_t>(a.domain) < static_cast<uint8_t>(b.domain);
              });

    VkBuffer prev_buf = VK_NULL_HANDLE;
    EBufferDomain prev_domain{};
    for (const auto &c : sorted)
    {
        if (c.dst == prev_buf && c.domain == prev_domain) continue;

        auto sa = domainToStageAccess(c.domain);

        VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        b.srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask  = sa.stages;
        b.dstAccessMask = sa.access;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.buffer = c.dst;
        b.offset = 0;
        b.size   = VK_WHOLE_SIZE;

        post_buffer_barriers_.push_back(b);
        prev_buf    = c.dst;
        prev_domain = c.domain;
    }

    // ── Image post-barriers: TRANSFER_DST→READ layout ───────────────
    for (const auto &c : image_copies_)
    {
        auto sa = domainToStageAccess(c.domain);

        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask  = sa.stages;
        b.dstAccessMask = sa.access;
        b.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout     = c.new_layout;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image         = c.dst;
        b.subresourceRange = {
            c.subresource.aspectMask,
            c.subresource.mipLevel, 1,
            c.subresource.baseArrayLayer, c.subresource.layerCount,
        };

        post_image_barriers_.push_back(b);
    }

    // ── QFOT acquire barriers ───────────────────────────────────────
    for (const auto &q : qfot_acquires_)
    {
        auto sa = domainToStageAccess(q.domain);

        if (q.kind == QFOTAcquireRequest::Kind::Buffer)
        {
            VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            b.srcStageMask        = VK_PIPELINE_STAGE_2_NONE;
            b.srcAccessMask       = VK_ACCESS_2_NONE;
            b.dstStageMask        = sa.stages;
            b.dstAccessMask       = sa.access;
            b.srcQueueFamilyIndex = q.src_family;
            b.dstQueueFamilyIndex = q.dst_family;
            b.buffer              = q.buffer;
            b.offset              = q.buf_offset;
            b.size                = q.buf_size;

            post_buffer_barriers_.push_back(b);
        }
        else
        {
            VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b.srcStageMask        = VK_PIPELINE_STAGE_2_NONE;
            b.srcAccessMask       = VK_ACCESS_2_NONE;
            b.dstStageMask        = sa.stages;
            b.dstAccessMask       = sa.access;
            b.oldLayout           = q.img_layout;
            b.newLayout           = q.img_layout;
            b.srcQueueFamilyIndex = q.src_family;
            b.dstQueueFamilyIndex = q.dst_family;
            b.image               = q.image;
            b.subresourceRange    = q.img_range;

            post_image_barriers_.push_back(b);
        }
    }
}

void TransferScheduler::endTransfers(VkCommandBuffer cmd)
{
    buildPostBarriers();

    // Merge host-write and other extra barriers into the post batch.
    for (const auto &b : extra_post_barriers_)
        post_buffer_barriers_.push_back(b);

    if (post_buffer_barriers_.empty() && post_image_barriers_.empty())
        return;

    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.bufferMemoryBarrierCount = static_cast<uint32_t>(post_buffer_barriers_.size());
    dep.pBufferMemoryBarriers    = post_buffer_barriers_.data();
    dep.imageMemoryBarrierCount  = static_cast<uint32_t>(post_image_barriers_.size());
    dep.pImageMemoryBarriers     = post_image_barriers_.data();

    vkCmdPipelineBarrier2(cmd, &dep);
    ++last_barrier_count_;
}

// =====================================================================
//  Per-frame reset / staging retirement
// =====================================================================

void TransferScheduler::resetFrame(uint32_t frame_slot)
{
    buffer_copies_.clear();
    image_copies_.clear();
    qfot_acquires_.clear();
    extra_post_barriers_.clear();
    last_barrier_count_ = 0;
    last_copy_count_    = 0;
    ring_.resetSlot(frame_slot);
}

void TransferScheduler::retireStaging(uint32_t frame_slot)
{
    if (frame_slot < deferred_staging_.size())
    {
        // Move current overflow staging into the slot for deferred retirement.
        auto &slot = deferred_staging_[frame_slot];
        slot.clear();                       // Release previous frame's staging.
        slot = std::move(overflow_staging_); // Current overflow → slot.
    }
    overflow_staging_.clear();
}

} // namespace lux::render
