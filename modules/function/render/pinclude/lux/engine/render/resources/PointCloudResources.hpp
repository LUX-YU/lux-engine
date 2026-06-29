#pragma once

#include <lux/engine/render/resources/lifecycle/GPUResourceBase.hpp>
#include <lux/engine/render/FrameServices.hpp>
#include <lux/engine/render/resources/point_cloud/PointCloudGlobalBuffer.hpp>
#include <lux/engine/render/resources/point_cloud/GpuOctreeNodeBuffer.hpp>
#include <lux/engine/render/transfer/TransferScheduler.hpp>
#include <lux/engine/render/renderer/features/point_cloud/PointCloudGpuData.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace lux::render
{
    /**
     * @brief GPU resource adapter for the new-gen point cloud pipeline.
     *
     * Owns the two shared SSBOs used by all new-gen PCFeature* rendering modes:
     *  - PointCloudGlobalBuffer  — unified point SSBO (all chunks, all modes)
     *  - GpuOctreeNodeBuffer     — per-node metadata for compute-cull modes
     *
     * Registered in GPUResourceRegistry under EGPUResourceType::PointCloud so
     * that any PCFeature* can access these buffers via:
     * @code
     *   auto* pc = ctx.gpuResources().getResource<PointCloudResources>();
     *   auto& global_buf = pc->globalBuffer();
     * @endcode
     *
     * Initialization (call once before adding any PCFeature*):
     * @code
     *   pc->init(ctx.vmaAllocator(), 4'000'000, 65'536);
     * @endcode
     * PCFeature* constructors will auto-initialize with their Config defaults
     * if this has not been called yet.
     */
    class LUX_FUNCTION_PUBLIC PointCloudResources final
        : public GPUResourceBase<PointCloudResources, EGPUResourceType::PointCloud>
        , public ISceneFrameService
    {
    public:
        PointCloudResources() = default;
        ~PointCloudResources() { shutdown(); }

        // Non-copyable, non-movable (wraps Vulkan handles)
        PointCloudResources(const PointCloudResources&)            = delete;
        PointCloudResources& operator=(const PointCloudResources&) = delete;
        PointCloudResources(PointCloudResources&&)                 = delete;
        PointCloudResources& operator=(PointCloudResources&&)      = delete;

        // ========== Lifecycle ==========

        /**
         * @brief Allocate the two shared SSBOs.
         * @param allocator   VMA allocator (from RenderContext::vmaAllocator())
         * @param max_points  Total point capacity (all chunks combined)
         * @param max_nodes   Maximum octree node count (for compute-cull modes)
         */
        bool init(VmaAllocator allocator, uint32_t max_points, uint32_t max_nodes)
        {
            if (initialized_) return true;

            if (!global_buf_.init(allocator, max_points))
                return false;

            if (!node_buf_.init(allocator, max_nodes))
            {
                global_buf_.shutdown();
                return false;
            }

            initialized_ = true;
            return true;
        }

        void shutdown()
        {
            if (!initialized_) return;
            node_buf_.shutdown();
            global_buf_.shutdown();
            initialized_ = false;
        }

        bool isInitialized() const noexcept { return initialized_; }

        // ========== Access ==========

        PointCloudGlobalBuffer&       globalBuffer() noexcept       { return global_buf_; }
        const PointCloudGlobalBuffer& globalBuffer() const noexcept { return global_buf_; }

        GpuOctreeNodeBuffer&          nodeBuffer()   noexcept       { return node_buf_; }
        const GpuOctreeNodeBuffer&    nodeBuffer()   const noexcept { return node_buf_; }

        // ========== Deferred Destroy Queue ==========

        void setDeferredQueue(DeferredDestroyQueue* q) noexcept
        {
            global_buf_.setDeferredQueue(q);
            node_buf_.setDeferredQueue(q);
        }

        void setRetireScheduler(FrameRetireScheduler* rs) noexcept
        {
            global_buf_.setRetireScheduler(rs);
        }

        void setRetireOwnerToken(FrameRetireScheduler::OwnerToken owner_token) noexcept
        {
            global_buf_.setRetireOwnerToken(owner_token);
        }

        // ========== Optional overrides ==========

        std::string getDebugInfo() const
        {
            if (!initialized_)
                return "PointCloudResources: not initialized";
            return "PointCloudResources: "
                 + std::to_string(node_buf_.nodeCount())     + " nodes, "
                 + std::to_string(global_buf_.usedPoints())  + " points";
        }

        // ========== Streaming upload queue ==========

        /// Queue a chunk for upload. Called from RenderServer handler thread.
        /// Data is copied into the pending queue; the source can be freed after return.
        void queueUpload(uint32_t chunk_id, std::span<const GpuPointVertex> data)
        {
            pending_ops_.push_back(PcOp{ PcOpType::Upload, chunk_id, {data.begin(), data.end()} });
        }

        /// Queue a full clear: frees all point slots and removes all octree nodes.
        /// Processed IN ARRIVAL ORDER in submitTransfers(): it only discards state
        /// established BEFORE it this frame — a uploadChunk()/resetChunk() queued
        /// AFTER the clearAll is processed normally (the documented contract). The
        /// freed point regions are returned to the pool deferred (after the current
        /// GPU serial retires) so an in-flight frame can still read them. See
        /// PointCloudGlobalBuffer::freeAllSlots().
        void queueClearAll()
        {
            pending_ops_.push_back(PcOp{ PcOpType::ClearAll, 0u, {} });
        }

        /// Queue a lightweight chunk reset: sets point_count=0 and removes the
        /// octree node, but keeps the slot allocation for reuse.
        void queueResetChunk(uint32_t chunk_id)
        {
            pending_ops_.push_back(PcOp{ PcOpType::Reset, chunk_id, {} });
        }

        bool hasPendingUploads() const noexcept
        {
            return !pending_ops_.empty() || node_buf_.hasPendingRemovals();
        }

        // ========== TransferScheduler integration ==========

        void setUseTransferScheduler(bool v) noexcept { use_transfer_scheduler_ = v; }
        bool usesTransferScheduler()  const noexcept { return use_transfer_scheduler_; }

        void submitTransfers(TransferScheduler& scheduler)
        {
            bool node_dirty = node_buf_.hasPendingRemovals();

            // Process commands IN ARRIVAL ORDER so a clearAll only discards what
            // was queued before it; an upload/reset queued after it this frame is
            // applied on top (the documented contract — #7). Reused octree slots
            // are scrubbed from the pending-zero list inside acquireIndex (#25),
            // so the end-of-pass flushRemovedNodes() zeros only slots that were
            // NOT re-uploaded and never clobbers a freshly-staged node.
            for (auto& op : pending_ops_)
            {
                switch (op.type)
                {
                case PcOpType::ClearAll:
                    global_buf_.freeAllSlots();
                    node_buf_.removeAllNodes();
                    node_dirty = true;
                    break;

                case PcOpType::Reset:
                    global_buf_.resetSlot(op.chunk_id);
                    node_buf_.removeNode(op.chunk_id);
                    node_dirty = true;
                    break;

                case PcOpType::Upload:
                {
                    const auto count = static_cast<uint32_t>(op.data.size());
                    if (count == 0) break;

                    // Skip the upload if capacity growth failed (VMA OOM or the
                    // uint32 point ceiling in allocOrGrow) — otherwise upload()
                    // would write past the slot into the neighbouring chunk. (C-2)
                    if (!global_buf_.ensureSlotCapacity(op.chunk_id, count, scheduler))
                        break;
                    global_buf_.upload(op.chunk_id,
                        std::span<const GpuPointVertex>(op.data), scheduler);

                    auto slot_opt = global_buf_.getSlot(op.chunk_id);
                    if (slot_opt)
                    {
                        constexpr float fmax =  std::numeric_limits<float>::max();
                        constexpr float fmin = -std::numeric_limits<float>::max();
                        float bmin[3] = { fmax, fmax, fmax };
                        float bmax[3] = { fmin, fmin, fmin };
                        for (const auto& v : op.data)
                        {
                            bmin[0] = std::min(bmin[0], v.x);
                            bmin[1] = std::min(bmin[1], v.y);
                            bmin[2] = std::min(bmin[2], v.z);
                            bmax[0] = std::max(bmax[0], v.x);
                            bmax[1] = std::max(bmax[1], v.y);
                            bmax[2] = std::max(bmax[2], v.z);
                        }

                        GpuOctreeNode node{};
                        std::memcpy(node.bbox_min, bmin, sizeof(bmin));
                        std::memcpy(node.bbox_max, bmax, sizeof(bmax));
                        node.first_point  = slot_opt->first_point;
                        node.point_count  = count;
                        node.stream_state = 2;
                        node.lod_error    = 1000.0f;

                        node_buf_.upsertNode(op.chunk_id, node, scheduler);
                        node_dirty = true;
                    }
                    break;
                }
                }
            }
            pending_ops_.clear();

            if (node_buf_.hasPendingRemovals())
            {
                node_buf_.flushRemovedNodes(scheduler);
                node_dirty = true;
            }

            if (node_dirty)
                node_buf_.flushNodeCount(scheduler);
        }

    private:
        enum class PcOpType : uint8_t { Upload, Reset, ClearAll };

        // One ordered command stream. Keeping uploads, resets and clear-alls in a
        // single arrival-ordered queue (rather than three separate ones drained
        // clear-first) is what lets clearAll respect the "later same-frame uploads
        // survive" contract — see queueClearAll (#7).
        struct PcOp
        {
            PcOpType                    type;
            uint32_t                    chunk_id{0};   ///< Upload / Reset
            std::vector<GpuPointVertex> data;          ///< Upload only
        };

        PointCloudGlobalBuffer          global_buf_;
        GpuOctreeNodeBuffer             node_buf_;
        std::vector<PcOp>               pending_ops_;
        bool                            use_transfer_scheduler_{false};
    };

} // namespace lux::render
