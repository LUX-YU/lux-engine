#pragma once

#include <NRI.h>

#include <Extensions/NRIDeviceCreation.h>

#include <lux/engine/render/visibility.h>

#include <functional>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/extensions/device/DescriptorTable.hpp>
#include <lux/engine/render/extensions/device/SubmissionQueue.hpp>
#include <lux/engine/render/extensions/storage/Binding.hpp>
#include <lux/engine/render/runtime/DeviceOptions.hpp>

namespace lux::render::detail
{
    struct DeviceBackend;
    class CompiledPrograms;
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
        bool multi_draw_indirect = false;
        bool indirect_first_instance = false;
        bool buffer_descriptors = false;
        bool variable_descriptor_count = false;
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
            std::uint32_t temporary_capacity = 3
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
            SubmissionFrontiers frontiers;
            for (std::size_t q = 0; q < physical_queues_.size(); ++q)
            {
                frontiers.serials[q] = physical_queues_[q]->submitted();
            }
            deferRelease(frontiers, std::move(release));
        }

        template <class Release> void deferRelease(SubmissionQueue& queue, Release release) noexcept
        {
            SubmissionFrontiers frontiers;
            frontiers.serials[queueIndex(queue)] = queue.submitted();
            deferRelease(frontiers, std::move(release));
        }

        // Explicit accepted values, indexed by physical queue. Zero means no dependency.
        template <class Release> void deferRelease(SubmissionFrontiers frontiers, Release release) noexcept
        {
            std::array<std::size_t, 3> pending;
            std::size_t count = 0;
            for (std::size_t q = 0; q < physical_queues_.size(); ++q)
            {
                if (frontiers.serials[q] != 0)
                {
                    pending[count++] = q;
                }
            }
            const auto wait = [&](std::size_t index)
            {
                const auto q = pending[index];
                return physical_queues_[q]->whenSubmissionCompletes(frontiers.serials[q]);
            };
            auto complete = [release = std::move(release)](auto... results) mutable noexcept
            {
                (requireCleanupComplete(results), ...);
                std::invoke(release);
            };
            switch (count)
            {
            case 1:
                stdexec::spawn(wait(0) | stdexec::then(std::move(complete)), cleanup_scope_.get_token());
                break;
            case 2:
                stdexec::spawn(
                    stdexec::when_all(wait(0), wait(1)) | stdexec::then(std::move(complete)),
                    cleanup_scope_.get_token()
                );
                break;
            case 3:
                stdexec::spawn(
                    stdexec::when_all(wait(0), wait(1), wait(2)) | stdexec::then(std::move(complete)),
                    cleanup_scope_.get_token()
                );
                break;
            default:
                stdexec::spawn(stdexec::just() | stdexec::then(std::move(complete)), cleanup_scope_.get_token());
            }
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
        // Reusable recording is legal only after the owning FIF slice has completed all prior GPU use.
        [[nodiscard]] nri::Result beginCommandBuffer(nri::CommandBuffer&, bool reusable) const noexcept;
        [[nodiscard]] nri::Result resetCommandAllocator(nri::CommandAllocator&) const noexcept;
        [[nodiscard]] nri::Result flushMappedRange(nri::Buffer&, std::uint64_t, std::uint64_t) const noexcept;
        [[nodiscard]] nri::Result invalidateMappedRange(nri::Buffer&, std::uint64_t, std::uint64_t) const noexcept;

        [[nodiscard]] SubmissionQueue& queue(EQueueRole role = EQueueRole::GRAPHICS) noexcept
        {
            return *physical_queues_[queue_roles_[static_cast<std::size_t>(role)]];
        }

        // Device-wide native program cache; callers hold explicit logical uses, never native ownership.
        [[nodiscard]] CompiledPrograms& programs() noexcept { return *programs_; }

        [[nodiscard]] DescriptorTable& descriptors() noexcept
        {
            return descriptor_table_;
        }

        [[nodiscard]] const DescriptorTable& descriptors() const noexcept
        {
            return descriptor_table_;
        }

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

        // Surface creation requires an enabled native presentation-retirement contract.
        [[nodiscard]] bool supportsPresentRetirement() const noexcept;
        // Caller excludes new use and has settled accepted GPU work. Cold, bounded, one-shot.
        [[nodiscard]] nri::Result
        retireSwapChain(
            nri::SwapChain& swap_chain,
            nri::Fence* unconsumed_acquire,
            std::uint64_t timeout_ns
        ) const noexcept;

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
        void releaseBackendData() noexcept;
        struct BackendData;

        // Declared before all dependent members, destroyed after their complete cleanup.
        nri::Device* device_ = nullptr;
        nri::CoreInterface nri_core_{};
        BackendData* backend_ = nullptr;
        std::array<nri::FormatSupportBits, static_cast<std::size_t>(nri::Format::MAX_NUM)> format_support_{};
        EnabledDeviceFeatures enabled_features_{};
        std::uint64_t upload_alignment_ = 4;
        std::uint32_t vertex_stride_limit_ = 0, vertex_offset_limit_ = 0;
        std::vector<std::unique_ptr<SubmissionQueue>> physical_queues_;
        std::array<std::uint8_t, 3> queue_roles_{};
        // External producers settle before close; descriptors and queues are explicitly destroyed before Device.
        DescriptorTable descriptor_table_;
        std::unique_ptr<CompiledPrograms> programs_;
        stdexec::simple_counting_scope cleanup_scope_;
        bool closed_ = false;
    };
} // namespace lux::render::detail
