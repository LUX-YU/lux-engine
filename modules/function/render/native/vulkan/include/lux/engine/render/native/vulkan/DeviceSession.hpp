#pragma once
#include <lux/engine/core/execution/Scheduler.hpp>
#include <lux/engine/gapi/vk/LogicalDevice.hpp>
#include <lux/engine/gapi/vk/PhysicalDevice.hpp>

#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/native/vulkan/retirement/Retirement.hpp>
#include <lux/engine/render/resource/AsyncWork.hpp>
#include <lux/engine/render/resource/Binding.hpp>
#include <lux/engine/render/resource/ResourceRegistry.hpp>
#include <lux/engine/render/resource/TransferOptions.hpp>

namespace lux::render::vulkan
{
    class CompiledGraph;
    class DescriptorArray;
    class ExecutionContext;
    class Transfer;
    class UploadContext;
    class StagingArena;
    struct ImageTransferRegion;
    struct TransferStats;

    // Stable Native lifetime root. Bootstrap owns InstanceContext and outlives this object.
    // Queue roles that alias the same VkQueue borrow the same SubmissionQueue owner.
    class DeviceSession final
    {
    public:
        [[nodiscard]] static RenderResult<std::unique_ptr<DeviceSession>> create(
            const InstanceContext& instance,
            const DeviceOptions& options = {},
            std::uint32_t slots = 3
        ) noexcept;
        ~DeviceSession() noexcept;
        DeviceSession(const DeviceSession&) = delete;
        DeviceSession& operator=(const DeviceSession&) = delete;

        [[nodiscard]] RenderResult<void> drain() noexcept;
        [[nodiscard]] RenderResult<void> collect() noexcept;
        // Explicit RenderThread safe point, separate from polling and resource admission/collection.
        void dispatchCompletions() noexcept;
        [[nodiscard]] RenderResult<ExecutionContext*> prepare(
            std::shared_ptr<const CompiledGraph> code,
            const TargetDesc& target,
            StorageSet* scene = nullptr,
            StorageSet* view = nullptr,
            std::span<const StorageArgument> arguments = {}
        ) noexcept;
        [[nodiscard]] RenderResult<void> release(ExecutionContext* context) noexcept;

        [[nodiscard]] SubmissionQueue& queue(EQueueRole role = EQueueRole::GRAPHICS) noexcept
        {
            return *physical_queues_[queue_roles_[static_cast<std::size_t>(role)]];
        }

        [[nodiscard]] RetirementQueue& retirement() noexcept
        {
            return *retirement_;
        }

        [[nodiscard]] RenderResult<std::uint32_t> sampledImageCapacity() const noexcept;
        [[nodiscard]] RenderResult<std::shared_ptr<DescriptorArray>> sampledImageArray() noexcept;

        [[nodiscard]] VulkanAllocator& allocator() noexcept
        {
            return allocator_;
        }

        [[nodiscard]] const gapi::vk::PhysicalDevice& physicalDevice() const noexcept
        {
            return physical_device_;
        }

        [[nodiscard]] VkDevice nativeDevice() const noexcept
        {
            return logical_device_.handle();
        }

        [[nodiscard]] const EnabledDeviceFeatures& enabledFeatures() const noexcept
        {
            return enabled_features_;
        }

        [[nodiscard]] const SubmissionQueue& queue(EQueueRole role = EQueueRole::GRAPHICS) const noexcept
        {
            return *physical_queues_[queue_roles_[static_cast<std::size_t>(role)]];
        }

        // Boundary lookup compares stable owner identity before inspecting the ticket's queue.
        [[nodiscard]] SubmissionQueue* queueFor(SubmissionTicket ticket) noexcept;
        [[nodiscard]] bool deviceLost() const noexcept;
        [[nodiscard]] RenderResult<void> configureUploads(const TransferOptions&, execution::CpuScheduler) noexcept;
        [[nodiscard]] RenderResult<std::unique_ptr<Transfer>> uploadImage(
            const ImageDescription&,
            std::span<const std::byte>,
            std::span<const ImageTransferRegion>,
            std::shared_ptr<const Image> clone = {},
            bool graphics_queue = false,
            SubmissionRequirements* clone_last_use = nullptr
        ) noexcept;
        [[nodiscard]] RenderResult<std::unique_ptr<Transfer>> uploadBuffer(
            std::span<const std::byte>,
            VkBufferUsageFlags usage
        ) noexcept;
        [[nodiscard]] TransferStats uploadStats() const noexcept;

        [[nodiscard]] UploadContext& uploads() noexcept
        {
            return *uploads_;
        }

        [[nodiscard]] ResourceRegistry& resources() noexcept
        {
            return resources_;
        }

        [[nodiscard]] const ResourceRegistry& resources() const noexcept
        {
            return resources_;
        }

        [[nodiscard]] StorageSet& globalStorages() noexcept
        {
            return global_storages_;
        }

    private:
        friend struct NativeMemoryAudit;
        friend class Transfer;
        friend class UploadContext;
        friend class ExecutionContext;
        DeviceSession(
            gapi::vk::PhysicalDevice physical,
            gapi::vk::LogicalDevice logical,
            EnabledDeviceFeatures enabled,
            VulkanAllocator allocator
        ) noexcept;
        gapi::vk::PhysicalDevice physical_device_;
        gapi::vk::LogicalDevice logical_device_;
        EnabledDeviceFeatures enabled_features_;
        VulkanAllocator allocator_;
        std::vector<std::unique_ptr<SubmissionQueue>> physical_queues_;
        std::array<std::uint8_t, 3> queue_roles_{};
        std::unique_ptr<RetirementQueue> retirement_;
        // CPU Context owners; accepted batches independently retain their physical resource sets.
        // Member destruction is explicitly ordered in the destructor.
        std::vector<std::unique_ptr<ExecutionContext>> contexts_;
        std::unique_ptr<StagingArena> staging_arena_;
        std::unique_ptr<UploadContext> uploads_;
        ResourceRegistry resources_;
        StorageSet global_storages_;
        std::weak_ptr<DescriptorArray> sampled_images_;
    };
} // namespace lux::render::vulkan
