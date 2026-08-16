#pragma once
/**
 * @file TrajectoryResources.hpp
 * @brief GPU resource container for trajectory rendering.
 *
 * Owns the shared vertex buffer used by all trajectory feature modes.
 * Registered in the scene via sceneRegistry().emplace<TrajectoryResources>().
 */

#include <lux/engine/render/gpu/lifecycle/GPUResourceBase.hpp>
#include <lux/engine/render/core/FrameServices.hpp>
#include <lux/engine/render/resources/TrajectoryGlobalBuffer.hpp>
#include <lux/engine/render/gpu/transfer/TransferScheduler.hpp>
#include <lux/engine/render/resources/TrajectoryGpuData.hpp>
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>   // TrajectoryHandle
#include <lux/cxx/container/SparseSet.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace lux::render
{
    class TrajectoryResources final
        : public GPUResourceBase<TrajectoryResources, EGPUResourceType::Trajectory>
        // (此前还继承 IFrameService,但**一个钩子都没重写** —— 每帧被遍历到,
        //  执行的是基类空实现。纯死重量,已摘除;零行为变化。)
    {
    public:
        TrajectoryResources() = default;
        ~TrajectoryResources() { shutdown(); }

        TrajectoryResources(const TrajectoryResources &) = delete;
        TrajectoryResources &operator=(const TrajectoryResources &) = delete;
        TrajectoryResources(TrajectoryResources &&) = delete;
        TrajectoryResources &operator=(TrajectoryResources &&) = delete;

        // ========== Lifecycle ==========
        bool init(VmaAllocator allocator, uint32_t max_vertices)
        {
            if (initialized_)
                return true;

            if (!global_buf_.init(allocator, max_vertices))
                return false;

            initialized_ = true;
            return true;
        }

        void shutdown()
        {
            if (!initialized_)
                return;
            global_buf_.shutdown();
            live_trajectories_.clear();
            generations_.clear();
            pending_remove_flags_.clear();
            initialized_ = false;
        }

        bool isInitialized() const noexcept { return initialized_; }

        // ========== Access ==========

        TrajectoryGlobalBuffer &globalBuffer() noexcept { return global_buf_; }
        const TrajectoryGlobalBuffer &globalBuffer() const noexcept { return global_buf_; }

        // ========== Deferred Destroy Queue ==========

        void setDeferredQueue(DeferredDestroyQueue* q) noexcept
        {
            global_buf_.setDeferredQueue(q);
        }

        void setRetireScheduler(FrameRetireScheduler* rs) noexcept
        {
            global_buf_.setRetireScheduler(rs);
        }

        void setRetireOwnerToken(FrameRetireScheduler::OwnerToken owner_token) noexcept
        {
            global_buf_.setRetireOwnerToken(owner_token);
        }

        // ========== Streaming upload queue ==========

        /// Create a trajectory handle and queue initial data upload.
        [[nodiscard]] TrajectoryHandle createTrajectory(std::span<const GpuTrajectoryVertex> data)
        {
            if (!initialized_)
                return TrajectoryHandle::invalid();

            const uint32_t trajectory_index = live_trajectories_.emplace(static_cast<uint8_t>(1));
            if (trajectory_index >= generations_.size())
            {
                generations_.resize(trajectory_index + 1, 0u);
                pending_remove_flags_.resize(trajectory_index + 1, 0u);
            }

            TrajectoryHandle handle{trajectory_index, generations_[trajectory_index]};
            pending_ops_.push_back({TrajOpKind::Upload, trajectory_index, {data.begin(), data.end()}});
            return handle;
        }

        /// Queue an append operation. Data is copied; the source can be freed after return.
        [[nodiscard]] bool queueAppend(TrajectoryHandle trajectory, std::span<const GpuTrajectoryVertex> data)
        {
            if (!isHandleAlive(trajectory))
                return false;
            pending_ops_.push_back({TrajOpKind::Append, trajectory.index, {data.begin(), data.end()}});
            return true;
        }

        /// Queue a trajectory clear (reset vertex count to 0).
        [[nodiscard]] bool queueClear(TrajectoryHandle trajectory)
        {
            if (!isHandleAlive(trajectory))
                return false;
            pending_ops_.push_back({TrajOpKind::Clear, trajectory.index, {}});
            return true;
        }

        /// Queue a trajectory removal (free its slot).
        [[nodiscard]] bool queueRemove(TrajectoryHandle trajectory)
        {
            if (!isHandleAlive(trajectory))
                return false;

            pending_remove_flags_[trajectory.index] = 1u;
            pending_ops_.push_back({TrajOpKind::Remove, trajectory.index, {}});
            return true;
        }

        /// Queue an atomic replace: clear + re-upload in the same frame (no flicker).
        [[nodiscard]] bool queueReplace(TrajectoryHandle trajectory,
                                        std::span<const GpuTrajectoryVertex> data)
        {
            if (!isHandleAlive(trajectory))
                return false;
            pending_ops_.push_back({TrajOpKind::Replace, trajectory.index, {data.begin(), data.end()}});
            return true;
        }

        [[nodiscard]] bool isHandleAlive(TrajectoryHandle trajectory) const noexcept
        {
            if (!trajectory.isValid())
                return false;

            const uint32_t trajectory_index = trajectory.index;
            if (trajectory_index >= generations_.size())
                return false;
            if (generations_[trajectory_index] != trajectory.gen)
                return false;
            if (!live_trajectories_.contains(trajectory_index))
                return false;
            if (trajectory_index < pending_remove_flags_.size() &&
                pending_remove_flags_[trajectory_index] != 0u)
                return false;

            return true;
        }

        bool hasPendingUploads() const noexcept
        {
            return !pending_ops_.empty();
        }

        // ========== TransferScheduler integration ==========

        void setUseTransferScheduler(bool v) noexcept { use_transfer_scheduler_ = v; }
        bool usesTransferScheduler()  const noexcept { return use_transfer_scheduler_; }

        void submitTransfers(TransferScheduler& scheduler)
        {
            // Replay the op-log in client ISSUE order. A fixed phase order
            // (clears -> replaces -> uploads -> removes) resolved same-frame
            // conflicting ops on one trajectory wrongly (append-then-replace ended
            // as replace+append; append-then-clear still showed the append). (C-11)
            for (auto& op : pending_ops_)
            {
                const auto count = static_cast<uint32_t>(op.data.size());
                switch (op.kind)
                {
                case TrajOpKind::Clear:
                    global_buf_.clearTrajectory(op.trajectory_index);
                    break;

                case TrajOpKind::Replace:
                    // Atomic clear + re-upload (no flicker); clears even when empty.
                    global_buf_.clearTrajectory(op.trajectory_index);
                    if (count == 0) break;
                    // Skip the upload if capacity growth failed (C-2).
                    if (!global_buf_.ensureSlotCapacity(op.trajectory_index, count, scheduler))
                        break;
                    global_buf_.upload(op.trajectory_index,
                                       std::span<const GpuTrajectoryVertex>(op.data), scheduler);
                    break;

                case TrajOpKind::Upload:
                    if (count == 0) break;
                    if (!global_buf_.ensureSlotCapacity(op.trajectory_index, count, scheduler))
                        break;   // growth failed — skip, don't write OOB (C-2)
                    global_buf_.upload(op.trajectory_index,
                                       std::span<const GpuTrajectoryVertex>(op.data), scheduler);
                    break;

                case TrajOpKind::Append:
                {
                    if (count == 0) break;
                    auto existing = global_buf_.getSlot(op.trajectory_index);
                    if (!existing)
                    {
                        if (!global_buf_.ensureSlotCapacity(op.trajectory_index, count, scheduler))
                            break;   // growth failed — skip, don't write OOB (C-2)
                        global_buf_.upload(op.trajectory_index,
                                           std::span<const GpuTrajectoryVertex>(op.data), scheduler);
                    }
                    else
                    {
                        const uint32_t needed = existing->count + count;
                        if (needed > existing->capacity)
                            global_buf_.ensureSlotCapacity(op.trajectory_index, needed, scheduler);
                        global_buf_.append(op.trajectory_index,
                                           std::span<const GpuTrajectoryVertex>(op.data), scheduler);
                    }
                    break;
                }

                case TrajOpKind::Remove:
                    global_buf_.freeSlot(op.trajectory_index);
                    (void)live_trajectories_.erase(op.trajectory_index);
                    ++generations_[op.trajectory_index];
                    pending_remove_flags_[op.trajectory_index] = 0u;
                    break;
                }
            }
            pending_ops_.clear();
        }

    private:
        enum class TrajOpKind : uint8_t { Upload, Append, Clear, Replace, Remove };
        struct TrajOp
        {
            TrajOpKind kind;
            uint32_t   trajectory_index;
            std::vector<GpuTrajectoryVertex> data;  // empty for Clear / Remove
        };

        lux::cxx::OffsetAutoSparseSet<uint32_t, uint8_t> live_trajectories_;
        std::vector<uint32_t> generations_;
        std::vector<uint8_t> pending_remove_flags_;
        TrajectoryGlobalBuffer global_buf_;
        // Single ordered op-log so same-frame ops on one trajectory resolve in
        // client issue order (C-11). pending_remove_flags_ still gates ops issued
        // AFTER a remove in the same frame (isHandleAlive rejects them).
        std::vector<TrajOp>   pending_ops_;
        bool                  use_transfer_scheduler_{false};
    };

} // namespace lux::render
