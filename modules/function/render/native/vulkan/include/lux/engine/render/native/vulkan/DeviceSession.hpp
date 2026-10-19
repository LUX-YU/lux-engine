#pragma once
#include <lux/engine/core/execution/Scheduler.hpp>
#include <lux/engine/gapi/vk/LogicalDevice.hpp>
#include <lux/engine/gapi/vk/PhysicalDevice.hpp>

#include <functional>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/native/vulkan/transfer/Submission.hpp>
#include <lux/engine/render/resource/Binding.hpp>
#include <lux/engine/render/resource/ResourceRegistry.hpp>
#include <lux/engine/render/resource/TransferOptions.hpp>

namespace lux::render::vulkan
{
    class CompiledGraph;
    class DescriptorArray;
    class ExecutionContext;
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
        // External Context/Storage owners and upload producers must be settled before final close.
        void close() noexcept;

        // Start on the Native owner. Every queue wait is registered now; cleanup executes inline
        // in the last ordered completion callback. No resource is retained by the queue itself.
        template <class Release> void deferRelease(Release release) noexcept
        {
            auto complete = [release = std::move(release)](auto... results) mutable noexcept
            {
                (requireCleanupComplete(results), ...);
                std::invoke(release);
            };
            switch (physical_queues_.size())
            {
            case 1:
                stdexec::spawn(
                    physical_queues_[0]->whenIdle() | stdexec::then(std::move(complete)),
                    cleanup_scope_.get_token()
                );
                break;
            case 2:
                stdexec::spawn(
                    stdexec::when_all(physical_queues_[0]->whenIdle(), physical_queues_[1]->whenIdle()) |
                        stdexec::then(std::move(complete)),
                    cleanup_scope_.get_token()
                );
                break;
            case 3:
                stdexec::spawn(
                    stdexec::when_all(
                        physical_queues_[0]->whenIdle(),
                        physical_queues_[1]->whenIdle(),
                        physical_queues_[2]->whenIdle()
                    ) | stdexec::then(std::move(complete)),
                    cleanup_scope_.get_token()
                );
                break;
            default:
                // No queue exists only during rollback of device construction.
                complete();
            }
        }

        template <class Release> void deferRelease(SubmissionQueue& queue, Release release) noexcept
        {
            stdexec::spawn(
                queue.whenIdle() | stdexec::then(
                                       [release = std::move(release)](RenderResult<void> result) mutable noexcept
                                       {
                                           requireCleanupComplete(result);
                                           std::invoke(release);
                                       }
                                   ),
                cleanup_scope_.get_token()
            );
        }

        [[nodiscard]] RenderResult<void> collect() noexcept;
        // Explicit RenderThread safe point, separate from polling and resource admission/collection.
        void dispatchCompletions() noexcept;
        [[nodiscard]] RenderResult<std::unique_ptr<ExecutionContext>> prepare(
            const CompiledGraph& code,
            const TargetDesc& target,
            StorageSet* scene = nullptr,
            StorageSet* view = nullptr,
            std::span<const StorageArgument> arguments = {}
        ) noexcept;

        [[nodiscard]] SubmissionQueue& queue(EQueueRole role = EQueueRole::GRAPHICS) noexcept
        {
            return *physical_queues_[queue_roles_[static_cast<std::size_t>(role)]];
        }

        [[nodiscard]] RenderResult<std::uint32_t> sampledImageCapacity() const noexcept;
        [[nodiscard]] RenderResult<DescriptorArray*> sampledImageArray() noexcept;

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

        [[nodiscard]] bool deviceLost() const noexcept;
        [[nodiscard]] RenderResult<void> configureUploads(const TransferOptions&, execution::CpuScheduler) noexcept;
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
        friend class UploadContext;
        friend class ExecutionContext;
        static void requireCleanupComplete(const RenderResult<void>&) noexcept;
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
        // Reverse destruction: Sets -> Registry -> retired objects -> upload measurements/staging ->
        // descriptor table -> physical Queues -> Allocator -> LogicalDevice.
        std::unique_ptr<DescriptorArray> sampled_images_;
        std::unique_ptr<StagingArena> staging_arena_;
        std::unique_ptr<UploadContext> uploads_;
        stdexec::simple_counting_scope cleanup_scope_;
        bool closed_ = false;
        ResourceRegistry resources_;
        StorageSet global_storages_;
    };
} // namespace lux::render::vulkan
