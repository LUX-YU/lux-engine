#pragma once

#include <NRI.h>

#include <Extensions/NRIDeviceCreation.h>
#include <Extensions/NRIRayTracing.h>
#include <Extensions/NRIWrapperVK.h>

#include <lux/engine/core/execution/Scheduler.hpp>
#include <lux/engine/render/visibility.h>

#include <functional>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/device/Submission.hpp>
#include <lux/engine/render/resource/Binding.hpp>
#include <lux/engine/render/resource/ResourceRegistry.hpp>
#include <lux/engine/render/resource/TransferOptions.hpp>
#include <lux/engine/render/runtime/DeviceOptions.hpp>

namespace lux::render::detail
{
    class CompiledGraph;
    class DescriptorArray;
    class ExecutionContext;
    class OffscreenTarget;
    class UploadContext;
    class StagingArena;
    struct ImageTransferRegion;
    struct TransferStats;
    struct StorageBindingOps;

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
            std::uint32_t slots = 3,
            std::uint32_t frames_in_flight = 2
        ) noexcept;
        ~DeviceSession() noexcept;
        DeviceSession(const DeviceSession&) = delete;
        DeviceSession& operator=(const DeviceSession&) = delete;

        // Embedded in the existing upload admission operation; this list owns no operation state.
        struct FrameWriteWaiter final
        {
            FrameWriteWaiter* next = nullptr;
            void* operation = nullptr;
            bool (*stopping)(void*) noexcept = nullptr;
            void (*complete)(void*) noexcept = nullptr;
        };

        [[nodiscard]] bool tryBeginFrameWrite(std::uint32_t& slot) noexcept;
        void finishFrameWrite(std::uint32_t slot) noexcept;
        void enqueueFrameWrite(FrameWriteWaiter&) noexcept;
        void afterFrameSubmit(void* receiver, void (*complete)(void*, RenderResult<void>) noexcept) noexcept;

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

        // Call after collect/dispatch on the device thread. Contexts are borrowed for this call only.
        [[nodiscard]] RenderResult<std::uint32_t> render(std::span<ExecutionContext* const> contexts) noexcept;
        [[nodiscard]] RenderResult<void> collect() noexcept;
        // Explicit RenderThread safe point, separate from polling and resource admission/collection.
        void dispatchCompletions() noexcept;
        [[nodiscard]] RenderResult<std::unique_ptr<ExecutionContext>> prepare(
            const CompiledGraph& code,
            OffscreenTarget& target,
            StorageSet* scene = nullptr,
            StorageSet* view = nullptr,
            std::span<const StorageArgument> arguments = {}
        ) noexcept;

        [[nodiscard]] std::uint32_t framesInFlight() const noexcept
        {
            return frame_count_;
        }

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

        [[nodiscard]] const nri::CoreInterface& api() const noexcept
        {
            return api_;
        }

        [[nodiscard]] const nri::WrapperVKInterface& vk() const noexcept
        {
            return vk_;
        }

        [[nodiscard]] const nri::DeviceDesc& description() const noexcept
        {
            return api_.GetDeviceDesc(*device_);
        }

        [[nodiscard]] std::span<const nri::FormatSupportBits> formatSupport() const noexcept
        {
            return format_support_;
        }

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

        [[nodiscard]] bool pagedInstances() const noexcept
        {
            return paged_instances_;
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
        [[nodiscard]] RenderResult<bool> waitForProgress(std::uint64_t timeout_ns) noexcept;
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
            std::unique_ptr<nri::Device, decltype(&nri::nriDestroyDevice)>,
            nri::CoreInterface,
            nri::WrapperVKInterface,
            EnabledDeviceFeatures
        ) noexcept;
        enum class EFramePhase : std::uint8_t
        {
            FREE,
            COLLECTING,
            PREPARING,
            SUBMITTED
        };

        struct FrameSlot final
        {
            EFramePhase phase = EFramePhase::FREE;
            std::array<std::uint64_t, 3> accepted_serials{};
            std::array<SubmissionCallback, 3> completions;
            std::uint32_t pending_writes = 0;
            std::uint8_t pending_queues = 0;
        };

        void sealFrame(FrameSlot&) noexcept;
        void dispatchFrameWrites() noexcept;
        [[nodiscard]] bool backgroundUploadsAllowed() const noexcept;
        void acceptFrameSubmission(std::uint32_t, const SubmissionQueue&) noexcept;
        void finishFrameSubmission(RenderResult<void>) noexcept;
        std::array<FrameSlot, 3> frames_;
        std::uint32_t frame_count_ = 1;
        std::uint32_t next_frame_ = 0;
        std::vector<std::pair<void*, const StorageBindingOps*>> frame_storages_;
        std::vector<std::pair<void*, void (*)(void*, RenderResult<void>) noexcept>> frame_receivers_;
        FrameWriteWaiter* frame_waiters_ = nullptr;
        FrameWriteWaiter* frame_waiters_tail_ = nullptr;
        bool rendering_ = false; // Reserves one existing Staging page while actual frame consumers are active.

        // Declared before all dependent members, destroyed after their complete cleanup.
        std::unique_ptr<nri::Device, decltype(&nri::nriDestroyDevice)> device_{nullptr, &nri::nriDestroyDevice};
        nri::CoreInterface api_{};
        nri::WrapperVKInterface vk_{};
        std::array<nri::FormatSupportBits, static_cast<std::size_t>(nri::Format::MAX_NUM)> format_support_{};
        EnabledDeviceFeatures enabled_features_{};
        bool paged_instances_ = false;
        std::uint64_t upload_alignment_ = 4;
        std::uint32_t vertex_stride_limit_ = 0, vertex_offset_limit_ = 0, bindless_capacity_ = 0;
        std::vector<std::unique_ptr<SubmissionQueue>> physical_queues_;
        std::array<std::uint8_t, 3> queue_roles_{};
        // Close settles Sets and cleanup before upload/staging and descriptors; queues outlive all callbacks.
        // The NRI Device is destroyed last by member order.
        std::unique_ptr<DescriptorArray> sampled_images_;
        std::unique_ptr<StagingArena> staging_arena_;
        std::unique_ptr<UploadContext> uploads_;
        stdexec::simple_counting_scope cleanup_scope_;
        bool closed_ = false;
        ResourceRegistry resources_;
        StorageSet global_storages_;
    };
} // namespace lux::render::detail
