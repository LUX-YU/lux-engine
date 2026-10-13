#pragma once

#include <lux/cxx/container/BasicSparseSet.hpp>
#include <lux/engine/render/runtime/NativeAssembly.hpp>
#include <lux/engine/render/runtime/ResourceDomain.hpp>

namespace lux::render::vulkan
{
    struct CompiledGraphEntry
    {
        RenderGraphId id;
        Graph code;
        std::vector<NativePassCommand> commands;
        std::vector<ProgramBindings> program_bindings;
        std::uint32_t width, height;
    };

    struct BindingExecution
    {
        bool occupied{};
        GraphInvocationData values;
        std::uint64_t revision{};
        std::optional<GraphSubmission> latest;
        std::uint64_t latest_frame{};
        bool scene_borrowed{};
    };

    // Independent View/FIF slots share immutable code. Native owns all queue tickets.
    struct GraphExecution
    {
        GraphExecutionContext native;
        std::vector<BindingExecution> bindings;
        std::vector<NativeTraceEvent> trace;
    };

    struct InstanceContext
    {
        VulkanInstance instance;
        [[nodiscard]] RenderResult<std::vector<VkPhysicalDevice>> physicalDevices() const noexcept;
    };

    struct DeviceContext
    {
        VulkanDevice device;
        VulkanAllocator allocator;
        std::array<std::unique_ptr<SubmissionQueue>, 3> queue_owners;
        std::array<SubmissionQueue*, 3> queues;
        // Cold GPU-safe point for final Session destruction, never a frame wait-idle.
        [[nodiscard]] RenderResult<void> drain() noexcept;
    };

    // GraphExecutionContext already owns per-job/FIF descriptor, staging and backing pools.
    // Command pools remain with the actual SubmissionQueue; no duplicate global pool is created.
    struct BufferFactory;

    struct ResourceContext
    {
        Sampler sampler;
        const VulkanAllocator& allocator;
        SubmissionQueue& initialization_queue;
        ResourceDomain scoped;
        [[nodiscard]] BufferFactory bufferFactory(VkDeviceSize size, VkBufferUsageFlags usage) const noexcept;
        cxx::SlotKeySparseSet<RenderGraphId, std::unique_ptr<GraphExecution>> executions;
    };

    // Stable single-device lifetime root. Borrows the bootstrap Instance; owns device/resource contexts and backing.
    // No worker or frame loop: Backend is the only executor.
    class DeviceSession
    {
    public:
        [[nodiscard]] static RenderResult<std::unique_ptr<DeviceSession>> create(
            const InstanceContext& host,
            const BackendOptions& options
        ) noexcept;
        static RenderResult<std::unique_ptr<DeviceSession>> create(InstanceContext&&, const BackendOptions&) = delete;
        ~DeviceSession() noexcept;
        DeviceSession(const DeviceSession&) = delete;
        DeviceSession& operator=(const DeviceSession&) = delete;

        const InstanceContext& instance;
        DeviceContext device;
        cxx::SlotMap<CompiledGraphEntry, RenderGraphTag> graphs;
        ResourceContext resources;

    private:
        DeviceSession(const InstanceContext& host, DeviceContext native, Sampler sampler) noexcept;
    };

    // Minimal typed resource factory: real R4 Buffer, not a replacement for specialized GPU storage.
    // This factory deliberately supports DEVICE access only; configuration equality is complete.
    struct BufferFactory
    {
        const VulkanAllocator& allocator;
        VkDeviceSize size;
        VkBufferUsageFlags usage;
        SubmissionQueue& queue;

        [[nodiscard]] std::optional<SubmissionTicket> readyTicket() const noexcept
        {
            return queue.lastSubmitted();
        }

        [[nodiscard]] RenderResult<std::unique_ptr<Buffer>> operator()() const noexcept;
        [[nodiscard]] bool isCompatible(const Buffer& buffer) const noexcept;
    };
} // namespace lux::render::vulkan
