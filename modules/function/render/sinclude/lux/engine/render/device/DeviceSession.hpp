#pragma once

#include <NRI.h>

#include <Extensions/NRIDeviceCreation.h>

#include <lux/engine/render/visibility.h>

#include <functional>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/device/SubmissionQueue.hpp>
#include <lux/engine/render/resource/Binding.hpp>
#include <lux/engine/render/runtime/DeviceOptions.hpp>

namespace lux::render::detail
{
    struct DeviceBackend;
    class DescriptorArray;
    struct StorageBindingOps;
    struct StorageFrameUse;

    // Backend capabilities used by the compiler, queried once from the successfully created device.
    struct EnabledDeviceFeatures final
    {
        bool multiview = false;
        bool local_read = false;
        bool texture_compression_bc = false;
        bool texture_descriptors = false;
        bool buffer_device_address = false;
    };

    // NRI owns the sole native Instance, Device and physical allocator. Aliased roles share
    // one SubmissionQueue and completion frontier. All physical cleanup uses cleanup_scope_.
    class LUX_RENDER_PUBLIC DeviceSession final
    {
    public:
        [[nodiscard]] static RenderResult<std::unique_ptr<DeviceSession>> create(
            const DeviceOptions& options,
            const nri::CallbackInterface& diagnostic,
            bool validation = false,
            std::uint32_t slots = 3
        ) noexcept;
        ~DeviceSession() noexcept;
        DeviceSession(const DeviceSession&) = delete;
        DeviceSession& operator=(const DeviceSession&) = delete;

        [[nodiscard]] RenderResult<void> drain() noexcept;
        // External Context/Storage owners and upload producers must be settled before final close.
        void close() noexcept;

        // Start on the device thread. Every queue wait is registered now; cleanup executes inline
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
                    physical_queues_[0]->whenSubmittedWorkCompletes() | stdexec::then(std::move(complete)),
                    cleanup_scope_.get_token()
                );
                break;
            case 2:
                stdexec::spawn(
                    stdexec::when_all(
                        physical_queues_[0]->whenSubmittedWorkCompletes(),
                        physical_queues_[1]->whenSubmittedWorkCompletes()
                    ) | stdexec::then(std::move(complete)),
                    cleanup_scope_.get_token()
                );
                break;
            case 3:
                stdexec::spawn(
                    stdexec::when_all(
                        physical_queues_[0]->whenSubmittedWorkCompletes(),
                        physical_queues_[1]->whenSubmittedWorkCompletes(),
                        physical_queues_[2]->whenSubmittedWorkCompletes()
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
                queue.whenSubmittedWorkCompletes() | stdexec::then(
                                       [release = std::move(release)](RenderResult<void> result) mutable noexcept
                                       {
                                           requireCleanupComplete(result);
                                           std::invoke(release);
                                       }
                                   ),
                cleanup_scope_.get_token()
            );
        }

        [[nodiscard]] RenderResult<void> pollQueues() noexcept;
        void dispatchQueueCompletions() noexcept;

        [[nodiscard]] std::size_t queueCount() const noexcept
        {
            return physical_queues_.size();
        }

        [[nodiscard]] SubmissionQueue& physicalQueue(std::size_t index) noexcept
        {
            return *physical_queues_[index];
        }

        [[nodiscard]] const SubmissionQueue& physicalQueue(std::size_t index) const noexcept
        {
            return *physical_queues_[index];
        }

        [[nodiscard]] std::size_t queueIndex(const SubmissionQueue&) const noexcept;

        [[nodiscard]] std::uint64_t uploadAlignment() const noexcept
        {
            return upload_alignment_;
        }

        // Backend operations absent from the core interface. No native API types escape.
        [[nodiscard]] nri::Result queryFence(nri::Fence&, std::uint64_t&) const noexcept;
        [[nodiscard]] nri::Result waitFence(nri::Fence&, std::uint64_t, std::uint64_t, bool&) const noexcept;
        [[nodiscard]] nri::Result resetCommandAllocator(nri::CommandAllocator&) const noexcept;
        [[nodiscard]] nri::Result flushMappedRange(nri::Buffer&, std::uint64_t, std::uint64_t) const noexcept;
        [[nodiscard]] nri::Result invalidateMappedRange(nri::Buffer&, std::uint64_t, std::uint64_t) const noexcept;

        [[nodiscard]] SubmissionQueue& queue(EQueueRole role = EQueueRole::GRAPHICS) noexcept
        {
            return *physical_queues_[queue_roles_[static_cast<std::size_t>(role)]];
        }

        [[nodiscard]] RenderResult<std::uint32_t> sampledImageCapacity() const noexcept;
        [[nodiscard]] RenderResult<DescriptorArray*> sampledImageArray() noexcept;

        [[nodiscard]] nri::Device& device() const noexcept
        {
            return *device_;
        }

        [[nodiscard]] const nri::CoreInterface& nriCore() const noexcept
        {
            return nri_core_;
        }

        [[nodiscard]] const nri::DeviceDesc& description() const noexcept
        {
            return nri_core_.GetDeviceDesc(*device_);
        }

        [[nodiscard]] std::span<const nri::FormatSupportBits> formatSupport() const noexcept
        {
            return format_support_;
        }

        // Cold creation qualification using the pinned backend's complete image-create key.
        [[nodiscard]] RenderResult<void> validateTexture(const nri::TextureDesc&) const noexcept;

        [[nodiscard]] nri::FormatSupportBits formatSupport(nri::Format format) const noexcept
        {
            return format_support_[static_cast<std::size_t>(format)];
        }

        [[nodiscard]] std::uint32_t vertexStrideLimit() const noexcept
        {
            return vertex_stride_limit_;
        }

        [[nodiscard]] std::uint32_t vertexOffsetLimit() const noexcept
        {
            return vertex_offset_limit_;
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
        [[nodiscard]] bool pendingGpuWork() const noexcept;

    private:
        friend struct DeviceBackend;
        static void requireCleanupComplete(const RenderResult<void>&) noexcept;
        DeviceSession() noexcept;

        // Declared before all dependent members, destroyed after their complete cleanup.
        nri::Device* device_ = nullptr;
        nri::CoreInterface nri_core_{};
        std::unique_ptr<DeviceBackend> backend_;
        std::array<nri::FormatSupportBits, static_cast<std::size_t>(nri::Format::MAX_NUM)> format_support_{};
        EnabledDeviceFeatures enabled_features_{};
        std::uint64_t upload_alignment_ = 4;
        std::uint32_t vertex_stride_limit_ = 0, vertex_offset_limit_ = 0, bindless_capacity_ = 0;
        std::vector<std::unique_ptr<SubmissionQueue>> physical_queues_;
        std::array<std::uint8_t, 3> queue_roles_{};
        // External producers settle before close; descriptors and queues are explicitly destroyed before Device.
        std::unique_ptr<DescriptorArray> sampled_images_;
        stdexec::simple_counting_scope cleanup_scope_;
        bool closed_ = false;
    };
} // namespace lux::render::detail
