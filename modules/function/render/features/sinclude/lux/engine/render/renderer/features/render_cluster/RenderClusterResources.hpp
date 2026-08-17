#pragma once

#include <lux/engine/function/render/client/features/render_cluster/RenderClusterOperation.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <vulkan/vulkan.h>

namespace lux::render
{
    class DeviceContext;
    class DeferredDestroyQueue;
    class InstanceResources;
    class LUX_FUNCTION_PUBLIC RenderClusterResources final
    {
    public:
        static constexpr std::uint32_t kPickTokenBits = 19u;
        static constexpr std::uint32_t kMaximumPickToken =
            (1u << kPickTokenBits) - 1u;
        enum class EVisibilityState : std::uint8_t
        {
            HIDDEN,
            FADING_IN,
            VISIBLE,
            FADING_OUT
        };

        enum class ETransitionAction : std::uint8_t
        {
            NONE,
            FADE_IN,
            FADE_OUT
        };

        struct Cluster final
        {
            UploadRenderClusterPayload header;
            std::vector<RenderClusterWireInstance> instances;
            std::vector<RenderObjectHandle> objects;
            std::vector<std::uint32_t> pick_tokens;
            bool visible{false};
            EVisibilityState visibility_state{EVisibilityState::HIDDEN};
            float transition_start_time{0.0f};
            float transition_duration{0.0f};
        };

        struct VisibilityChange final
        {
            RenderClusterWireId id;
            bool visible{false};
            ETransitionAction transition{ETransitionAction::NONE};
            float transition_start_time{0.0f};
            float transition_duration{0.0f};
            std::uint32_t transition_seed{1u};
        };

        struct CpuMemorySnapshot final
        {
            std::uint64_t capacity_bytes{0u};
            std::uint32_t allocation_count{0u};
        };

        /// GPU ABI for the coarse cluster pass. page_visible.xyz is relative to
        /// the RenderScene origin page; w is the CPU hierarchy-visibility bit.
        struct alignas(16) GpuCullCluster final
        {
            std::int32_t page_visible[4]{};
            float local_radius[4]{};
        };
        static_assert(sizeof(GpuCullCluster) == 32u);

        /// One resident static-instance slot and the cluster whose visibility
        /// controls it.  The expand pass consumes a dense array of these.
        struct GpuCullInstance final
        {
            std::uint32_t instance_slot{0u};
            std::uint32_t cluster_index{0u};
        };
        static_assert(sizeof(GpuCullInstance) == 8u);

        [[nodiscard]] bool accepts(
            RenderClusterWireId id,
            std::uint64_t revision) const noexcept;
        [[nodiscard]] bool validatesUpsert(
            const UploadRenderClusterPayload& header,
            std::span<const RenderClusterWireInstance> instances,
            std::size_t object_count,
            std::size_t pick_token_count) const;
        RenderClusterResources() = default;
        ~RenderClusterResources();

        [[nodiscard]] bool upsert(
            const UploadRenderClusterPayload& header,
            std::span<const RenderClusterWireInstance> instances,
            std::vector<RenderObjectHandle> objects = {},
            std::vector<std::uint32_t> pick_tokens = {});
        [[nodiscard]] bool remove(
            RenderClusterWireId id,
            std::uint64_t revision) noexcept;
        [[nodiscard]] const Cluster* find(RenderClusterWireId id) const;
        [[nodiscard]] std::vector<VisibilityChange> reconcileHierarchy(
            RenderClusterWireId family_parent,
            bool prefer_children = true,
            float scene_time = 0.0f,
            float transition_duration_seconds = 0.0f);
        [[nodiscard]] bool prefersChildren(
            RenderClusterWireId family_parent) const noexcept;
        [[nodiscard]] std::size_t transitionCount() const noexcept;
        [[nodiscard]] float transitionDurationSeconds() const noexcept
        {
            return transition_duration_seconds_;
        }
        [[nodiscard]] float hlodEnterErrorPixels() const noexcept
        {
            return hlod_enter_error_pixels_;
        }
        [[nodiscard]] float hlodExitErrorPixels() const noexcept
        {
            return hlod_exit_error_pixels_;
        }
        [[nodiscard]] static std::uint32_t transitionSeed(
            std::uint64_t stable_pick_id,
            RenderClusterWireId cluster,
            std::size_t instance_index) noexcept;
        [[nodiscard]] std::vector<RenderClusterWireId>
        hierarchyParents() const;
        void forEachObject(
            const std::function<void(RenderObjectHandle)>& visitor) const;
        void forEachVisibleObject(
            const std::function<void(RenderObjectHandle)>& visitor) const;
        void forEachVisiblePickObject(
            const std::function<void(
                RenderObjectHandle,
                std::uint32_t)>& visitor) const;

        [[nodiscard]] std::uint32_t allocatePickToken(std::uint64_t stable_pick_id);
        void cancelPickToken(std::uint32_t token) noexcept;
        [[nodiscard]] std::optional<std::uint64_t> resolvePickToken(
            std::uint32_t token) const noexcept;

        [[nodiscard]] bool canRebaseSceneOrigin(
            const std::int64_t origin_delta[3]) const noexcept;
        void rebaseSceneOrigin(
            const std::int64_t origin_delta[3]) noexcept;

        [[nodiscard]] bool initializePicking(
            DeviceContext& device,
            DeferredDestroyQueue& deferred_destroy,
            std::uint32_t frames_in_flight);
        void shutdownPicking() noexcept;
        void onPickingFrameBegin(std::uint32_t frame_index) noexcept;
        void requestPick(const RequestRenderClusterPickPayload& request)
            noexcept;
        [[nodiscard]] std::optional<RequestRenderClusterPickPayload>
        pickRequestForView(std::uint32_t view_index) const noexcept;
        void markPickSubmitted(
            std::uint32_t frame_index,
            const RequestRenderClusterPickPayload& request) noexcept;
        void failPick(
            const RequestRenderClusterPickPayload& request,
            ERenderPickStatus status = ERenderPickStatus::FAILED) noexcept;
        [[nodiscard]] RenderClusterPickReply pickResult(
            std::uint64_t request_generation) const noexcept;
        [[nodiscard]] std::uint32_t pickBufferCount() const noexcept;
        [[nodiscard]] VkBuffer pickBuffer(std::uint32_t index) const noexcept;

        [[nodiscard]] bool initializeGpuCulling(
            DeviceContext& device,
            DeferredDestroyQueue& deferred_destroy,
            std::uint32_t frames_in_flight,
            std::uint32_t initial_capacity);
        void shutdownGpuCulling() noexcept;
        /// Rebuild the fence-safe input slice for this frame. capacity_changed
        /// tells the feature to invalidate the graph because transient candidate
        /// buffers are sized from the same capacity.
        [[nodiscard]] bool prepareGpuCulling(
            std::uint32_t frame_index,
            const InstanceResources& instances,
            bool& capacity_changed);
        [[nodiscard]] std::uint32_t gpuCullBufferCount() const noexcept;
        [[nodiscard]] VkBuffer gpuCullClusterBuffer(
            std::uint32_t index) const noexcept;
        [[nodiscard]] VkBuffer gpuCullInstanceBuffer(
            std::uint32_t index) const noexcept;
        [[nodiscard]] VkBuffer gpuCandidateDispatchBuffer(
            std::uint32_t index) const noexcept;
        void markGpuCandidateSubmitted(std::uint32_t frame_index) noexcept;
        [[nodiscard]] std::uint32_t latestGpuCandidateCount() const noexcept
        {
            return latest_gpu_candidate_count_;
        }
        [[nodiscard]] std::uint32_t latestGpuCandidateRequestedCount()
            const noexcept
        {
            return latest_gpu_candidate_requested_count_;
        }
        [[nodiscard]] std::uint32_t latestGpuCandidateOverflowCount()
            const noexcept
        {
            return latest_gpu_candidate_overflow_count_;
        }
        [[nodiscard]] std::uint32_t latestGpuCandidateGroupCount() const noexcept
        {
            return latest_gpu_candidate_group_count_;
        }
        [[nodiscard]] bool hasGpuCandidateCount() const noexcept
        {
            return has_gpu_candidate_count_;
        }
        [[nodiscard]] bool hasValidGpuCandidateDispatch() const noexcept
        {
            return has_gpu_candidate_count_ &&
                gpu_candidate_dispatch_valid_;
        }
        [[nodiscard]] std::uint32_t gpuCullClusterCount(
            std::uint32_t frame_index) const noexcept;
        [[nodiscard]] std::uint32_t gpuCullInstanceCount(
            std::uint32_t frame_index) const noexcept;
        [[nodiscard]] std::uint32_t gpuCullCapacity() const noexcept
        {
            return gpu_cull_capacity_;
        }

        [[nodiscard]] std::size_t clusterCount() const noexcept
        {
            return clusters_.size();
        }

        [[nodiscard]] std::size_t instanceCount() const noexcept
        {
            return instance_count_;
        }

        [[nodiscard]] std::size_t visibleClusterCount() const noexcept
        {
            return visible_cluster_count_;
        }

        [[nodiscard]] std::size_t visibleInstanceCount() const noexcept
        {
            return visible_instance_count_;
        }

        /// Logical owner capacity of CPU containers. This intentionally does
        /// not claim allocator overhead or process RSS and excludes Vulkan
        /// allocations, which have their own budget accounting.
        [[nodiscard]] CpuMemorySnapshot cpuMemorySnapshot() const noexcept;

    private:
        [[nodiscard]] static std::string key(RenderClusterWireId id);
        void retirePickTokens(std::span<const std::uint32_t> tokens) noexcept;

        struct PickGpuSlot final
        {
            VkBuffer buffer{VK_NULL_HANDLE};
            void* allocation{nullptr};
            void* mapped{nullptr};
            bool submitted{false};
            RequestRenderClusterPickPayload request;
        };

        struct RetiredPickToken final
        {
            std::uint32_t token{0u};
            std::uint64_t retire_serial{0u};
        };

        struct GpuCullFrame final
        {
            VkBuffer cluster_buffer{VK_NULL_HANDLE};
            void* cluster_allocation{nullptr};
            void* cluster_mapped{nullptr};
            VkBuffer instance_buffer{VK_NULL_HANDLE};
            void* instance_allocation{nullptr};
            void* instance_mapped{nullptr};
            VkBuffer candidate_dispatch_buffer{VK_NULL_HANDLE};
            void* candidate_dispatch_allocation{nullptr};
            void* candidate_dispatch_mapped{nullptr};
            std::uint32_t cluster_count{0u};
            std::uint32_t instance_count{0u};
            std::uint64_t uploaded_revision{0u};
            bool candidate_submitted{false};
        };

        [[nodiscard]] bool allocateGpuCullFrames(
            std::uint32_t frames_in_flight,
            std::uint32_t capacity);
        void retireGpuCullFrames(
            std::vector<GpuCullFrame>& frames) noexcept;
        [[nodiscard]] bool rebuildGpuCullCanonical(const InstanceResources& instances);

        std::unordered_map<std::string, Cluster> clusters_;
        std::unordered_map<std::string, std::uint64_t> latest_revision_;
        std::unordered_map<std::string, std::unordered_set<std::string>>
            parent_members_;
        std::unordered_set<std::string> hierarchy_parents_;
        std::unordered_map<std::string, bool>
            hierarchy_prefer_children_;
        DeviceContext* pick_device_{nullptr};
        DeferredDestroyQueue* deferred_destroy_{nullptr};
        std::vector<PickGpuSlot> pick_gpu_slots_;
        std::vector<GpuCullFrame> gpu_cull_frames_;
        std::vector<GpuCullCluster> gpu_cull_clusters_;
        std::vector<GpuCullInstance> gpu_cull_instances_;
        std::uint32_t gpu_cull_capacity_{0u};
        std::uint64_t gpu_cull_revision_{1u};
        std::uint64_t gpu_cull_instance_layout_serial_{0u};
        bool gpu_cull_dirty_{true};
        std::uint32_t latest_gpu_candidate_count_{0u};
        std::uint32_t latest_gpu_candidate_requested_count_{0u};
        std::uint32_t latest_gpu_candidate_overflow_count_{0u};
        std::uint32_t latest_gpu_candidate_group_count_{0u};
        bool has_gpu_candidate_count_{false};
        bool gpu_candidate_dispatch_valid_{false};
        std::optional<RequestRenderClusterPickPayload> pending_pick_;
        RenderClusterPickReply latest_pick_{};
        std::unordered_map<std::uint32_t, std::uint64_t> pick_ids_;
        std::vector<std::uint32_t> free_pick_tokens_;
        std::vector<RetiredPickToken> retired_pick_tokens_;
        std::uint32_t next_pick_token_{1u};
        std::uint64_t pick_frame_serial_{0u};
        std::size_t instance_count_{0u};
        std::size_t visible_cluster_count_{0u};
        std::size_t visible_instance_count_{0u};
        float transition_duration_seconds_{0.35f};
        float hlod_enter_error_pixels_{2.5f};
        float hlod_exit_error_pixels_{1.5f};
    };
} // namespace lux::render
