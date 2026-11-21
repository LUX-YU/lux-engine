#pragma once

#include <exec/task.hpp>
#include <lux/cxx/execution/ManualContext.hpp>

#include <NRI.h>

#include <Extensions/NRIDeviceCreation.h>

#include <lux/engine/render/visibility.h>

#include <functional>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/extensions/device/DescriptorTable.hpp>
#include <lux/engine/render/extensions/device/SubmissionFrontiers.hpp>
#include <lux/engine/render/extensions/device/SubmissionQueue.hpp>
#include <lux/engine/render/extensions/resource/Binding.hpp>
#include <lux/engine/render/runtime/DeviceOptions.hpp>
#include <lux/engine/description/ShaderProgramArtifact.hpp>

namespace lux::render::detail
{
    struct DeviceBackend;
    class CompiledPrograms;

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
        rdesc::EShaderTarget shader_target = rdesc::EShaderTarget::SPIRV;

        [[nodiscard]] bool supports(const DeviceOptions& options) const noexcept
        {
            const bool has_descriptors = !options.texture_descriptors || texture_descriptors;
            const bool has_compression = !options.texture_compression_bc || texture_compression_bc;
            const bool has_addressing = !options.buffer_device_address || buffer_device_address;
            const bool has_views = !options.multiview || multiview;
            const bool has_local_read = !options.local_read || local_read;
            return has_descriptors && has_compression && has_addressing && has_views && has_local_read;
        }
    };

    // NRI owns the sole native Instance, Device and physical allocator. Aliased roles share
    // one SubmissionQueue and completion frontier. All physical cleanup uses cleanup_scope_.
    class LUX_RENDER_PUBLIC DeviceSession final
    {
    public:
        [[nodiscard]] bool usesDeviceAddresses() const noexcept { return use_device_addresses_; }
        [[nodiscard]] GraphQueueCosts graphQueueCosts() const noexcept { return graph_queue_costs_; }

        // Cold query values; an explicit DeviceOptions index uses this same NRI enumeration order.
        [[nodiscard]] static RenderResult<std::vector<nri::AdapterDesc>> enumerateAdapters() noexcept;
        [[nodiscard]] static RenderResult<std::unique_ptr<DeviceSession>> create(
            const DeviceOptions& options,
            const nri::CallbackInterface& diagnostic,
            bool validation,
            cxx::execution::ManualContext::Scheduler native_scheduler
        ) noexcept;
        ~DeviceSession() noexcept;
        DeviceSession(const DeviceSession&) = delete;
        DeviceSession& operator=(const DeviceSession&) = delete;

        [[nodiscard]] std::uint32_t resourceCapabilities() const noexcept;
        [[nodiscard]] RenderResult<void> drain() noexcept;
        // Shared by all logical queue roles in this Session's native host domain. Start only
        // from the control owner; CPU workers must first deliver back to that owner.
        [[nodiscard]] cxx::execution::ManualContext::Scheduler nativeScheduler() const noexcept;
        [[nodiscard]] RenderError terminalError() const noexcept;

        // Start on the device thread. Capture all accepted frontiers now and settle every queue before release.
        // No resource is retained by the queue itself.
        template <class Release> void deferRelease(Release release) noexcept
        {
            auto frontiers = makeFrontiers();
            for (std::size_t q = 0; q < physical_queues_.size(); ++q)
            {
                frontiers.accept(submissionPoint(q, physical_queues_[q]->submitted()));
            }
            deferRelease(frontiers, std::move(release));
        }

        template <class Release> void deferRelease(SubmissionQueue& queue, Release release) noexcept
        {
            auto frontiers = makeFrontiers();
            frontiers.accept(submissionPoint(queueIndex(queue), queue.submitted()));
            deferRelease(frontiers, std::move(release));
        }

        // Explicit accepted values, indexed by physical queue. Zero means no dependency.
        template <class Release> void deferRelease(SubmissionFrontiers frontiers, Release release) noexcept
        {
            auto complete = [this, release = std::move(release)](RenderResult<void> result) mutable noexcept
            {
                requireCleanupComplete(result);
                std::invoke(release);
            };
            stdexec::spawn(
                stdexec::starts_on(stdexec::inline_scheduler{}, waitFrontiers(std::move(frontiers))) |
                    stdexec::upon_error([](auto) noexcept -> RenderResult<void> { std::terminate(); }) |
                    stdexec::upon_stopped([]() noexcept -> RenderResult<void> { std::terminate(); }) |
                    stdexec::then(std::move(complete)),
                cleanup_scope_.get_token()
            );
        }

        [[nodiscard]] SubmissionFrontiers makeFrontiers() const noexcept
        {
            return {this, queueCount()};
        }

        void readCompleted(SubmissionFrontiers& completed) const noexcept
        {
            completed.clear();
            for (std::size_t queue = 0; queue < queueCount(); ++queue)
            {
                completed.accept(submissionPoint(queue, physicalQueue(queue).completed()));
            }
        }

        [[nodiscard]] SubmissionPoint submissionPoint(std::size_t queue, std::uint64_t serial) const noexcept
        {
            if (queue >= queueCount()) std::terminate();
            return {this, queue, serial};
        }

        [[nodiscard]] bool reached(const SubmissionFrontiers& frontiers) const noexcept
        {
            if (!frontiers.belongsTo(this, queueCount())) return false;
            for (std::size_t q = 0; q < queueCount(); ++q)
                if (frontiers.serials()[q] > physicalQueue(q).completed()) return false;
            return true;
        }

        [[nodiscard]] exec::task<RenderResult<void>> waitFrontiers(SubmissionFrontiers frontiers) noexcept;

        // Render safe point: publish completed and terminal work even when polling fails.
        [[nodiscard]] RenderResult<void> progressCompletions() noexcept;

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
        bool use_device_addresses_ = false;
        GraphQueueCosts graph_queue_costs_;
        friend struct DeviceBackend;
        void requireCleanupComplete(const RenderResult<void>&) noexcept;
        explicit DeviceSession(cxx::execution::ManualContext::Scheduler native_scheduler) noexcept;
        void releaseBackendData() noexcept;
        struct BackendData;

        cxx::execution::ManualContext::Scheduler native_scheduler_;
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
    };
} // namespace lux::render::detail
