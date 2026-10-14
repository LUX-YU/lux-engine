#pragma once

#include <lux/engine/render/native/vulkan/retirement/Retirement.hpp>
#include <lux/engine/render/features/Resources.hpp>

namespace lux::render::vulkan
{
    class CompiledGraph;
    class ExecutionContext;
    struct ScopeIdentity;
    struct ScopedBuffer;
    // Stable Native lifetime root. Bootstrap owns InstanceContext and outlives this object.
    // Queue roles that alias the same VkQueue borrow the same SubmissionQueue owner.
    class DeviceSession final
    {
    public:
        [[nodiscard]] static RenderResult<std::unique_ptr<DeviceSession>> create(
            const InstanceContext& instance,
            const DeviceOptions& options = {},
            std::uint32_t slots = 3,
            std::size_t retirement_capacity = 64
        ) noexcept;
        ~DeviceSession() noexcept;
        DeviceSession(const DeviceSession&) = delete;
        DeviceSession& operator=(const DeviceSession&) = delete;

        [[nodiscard]] RenderResult<void> drain() noexcept;
        [[nodiscard]] RenderResult<void> collect() noexcept;
        [[nodiscard]] RenderResult<ExecutionContext*> prepare(
            std::shared_ptr<const CompiledGraph> code, const TargetDesc& target,
            const ScopeIdentity& scope, std::span<const BufferInitializer> initializers
        ) noexcept;
        [[nodiscard]] RenderResult<void> release(ExecutionContext* context) noexcept;
        [[nodiscard]] std::size_t scopedBufferCount() const noexcept { return scoped_.size(); }
        [[nodiscard]] std::uint64_t scopedBufferCreations() const noexcept { return scoped_creations_; }
        [[nodiscard]] SubmissionQueue& queue(EQueueRole role = EQueueRole::GRAPHICS) noexcept
        {
            return *queues_[static_cast<std::size_t>(role)];
        }
        [[nodiscard]] RetirementQueue& retirement(EQueueRole role = EQueueRole::GRAPHICS) noexcept
        {
            return *retirements_[static_cast<std::size_t>(role)];
        }

        VulkanDevice device;
        VulkanAllocator allocator;

    private:
        DeviceSession(VulkanDevice device, VulkanAllocator allocator) noexcept;
        std::array<std::unique_ptr<SubmissionQueue>, 3> queue_owners_;
        std::array<SubmissionQueue*, 3> queues_{};
        std::array<std::optional<RetirementQueue>, 3> retirement_owners_;
        std::array<RetirementQueue*, 3> retirements_{};
        // Unique physical owners. Context leases borrow entries until their real
        // GPU ticket retires. Member destruction is explicitly ordered in the destructor.
        std::vector<std::unique_ptr<ScopedBuffer>> scoped_;
        std::vector<std::unique_ptr<ExecutionContext>> contexts_;
        std::uint64_t scoped_creations_ = 0;
    };
}
