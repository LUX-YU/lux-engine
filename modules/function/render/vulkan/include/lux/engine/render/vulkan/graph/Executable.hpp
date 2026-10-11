#pragma once

#include <lux/engine/render/graph/Bindings.hpp>
#include <lux/engine/render/vulkan/shader/Bindings.hpp>
#include <lux/engine/render/vulkan/transfer/Submission.hpp>

namespace lux::render::vulkan
{
    namespace detail
    {
        struct NativeGraphBacking;
    }

    struct DispatchCommand
    {
        std::uint32_t x{1}, y{1}, z{1};
        bool operator==(const DispatchCommand&) const noexcept = default;
    };

    struct DrawCommand
    {
        std::uint32_t vertices{3}, instances{1}, first_vertex{}, first_instance{};
        bool operator==(const DrawCommand&) const noexcept = default;
    };

    // Typed resource uses already specify source/destination and exact ranges.
    // Copy has no second list of resource identities to reconcile.
    struct CopyCommand
    {
        bool operator==(const CopyCommand&) const noexcept = default;
    };

    using VNativeCommand = std::variant<DispatchCommand, DrawCommand, CopyCommand>;

    struct NativePassCommand
    {
        GraphPassId pass;
        VNativeCommand command;
        EQueueRole queue;
        bool operator==(const NativePassCommand&) const noexcept = default;
    };

    struct NativeResourceState
    {
        VkPipelineStageFlags2 stages{};
        VkAccessFlags2 access{};
        VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
        std::uint32_t family{VK_QUEUE_FAMILY_IGNORED};
        bool operator==(const NativeResourceState&) const noexcept = default;
    };

    using VNativeImport = std::variant<std::reference_wrapper<const Buffer>, std::reference_wrapper<const Image>>;

    struct NativeViewRequirement
    {
        VkImageSubresourceRange range;
        VkImageViewType type;
    };

    struct NativeRangeState
    {
        VGraphRange range;
        NativeResourceState state;
        std::optional<SubmissionTicket> ready;
    };

    // Synchronous CPU borrow. External owner retains backing through the returned
    // receipt, and supplies completion evidence separately from logical epochs.
    struct NativeImportBinding
    {
        GraphResourceId resource;
        GraphBackingId identity;
        VNativeImport backing;
        NativeResourceState initial;
        std::optional<SubmissionTicket> ready;
        bool host_ready{false};
        // Pre-created views in importViews() order. The caller retains them through completion.
        // Empty reuses the original views only when the native image has not changed.
        std::span<const std::reference_wrapper<const ImageView>> views{};
        // Optional exact subresource states. If supplied they cover every native
        // cell once; otherwise initial/ready describe the whole imported backing.
        std::span<const NativeRangeState> ranges{};
    };

    struct NativeCompileInputs
    {
        const LogicalGraphPlan& logical;
        const VulkanDevice& device;
        const VulkanAllocator& allocator;
        std::array<SubmissionQueue*, 3> queues;
        std::span<const NativePassCommand> commands;
        std::span<const NativeImportBinding> imports;
        const GraphInvocationData& initial_values;
        std::span<const std::pair<GraphSampler, std::reference_wrapper<const Sampler>>> samplers{};
        std::uint32_t frame_capacity{2};
        bool alias{true};
    };

    struct NativeGraphStatistics
    {
        std::uint64_t allocated_bytes{};
        std::uint32_t allocations{}, aliased_resources{}, barriers{}, submissions{};
        // Cold compilation only: identity/candidate, resources, recipes, synchronization/admission.
        std::array<std::uint64_t, 4> compile_ns{};
    };

    // The submission ticket proves admission. Output is CPU-readable only after
    // its queue reports completion; a slot index or frame number proves neither.
    struct GraphSubmission
    {
        SubmissionTicket completion;
        std::uint32_t slot;
        std::uint32_t trace_count{};
    };

    enum class ENativeTrace
    {
        PASS,
        SKIP,
        TRANSITION,
        RELEASE,
        ACQUIRE,
        ALIAS
    };

    struct NativeTraceEvent
    {
        ENativeTrace operation;
        GraphPassId pass;
        PassKey pass_key;
        GraphResourceId resource;
        std::uint32_t use_index;
        VGraphRange range;
        EQueueRole queue;
        NativeResourceState before, after;
    };

    [[nodiscard]] std::string nativeTraceJson(std::span<const NativeTraceEvent> trace) noexcept;

    class ExecutableGraphPlan;
    [[nodiscard]] RenderResult<ExecutableGraphPlan> compileVulkanGraph(
        const NativeCompileInputs& inputs,
        std::vector<NativeShaderProgram> programs
    ) noexcept;

    class ExecutableGraphPlan
    {
    public:
        ~ExecutableGraphPlan() noexcept;
        ExecutableGraphPlan(ExecutableGraphPlan&&) noexcept;
        ExecutableGraphPlan& operator=(ExecutableGraphPlan&&) noexcept;
        ExecutableGraphPlan(const ExecutableGraphPlan&) = delete;
        ExecutableGraphPlan& operator=(const ExecutableGraphPlan&) = delete;

        [[nodiscard]] const LogicalGraphPlan& logical() const noexcept;
        [[nodiscard]] VkDevice device() const noexcept;
        // Latest completion join covers all successful invocations on all native queues.
        // A partial-submit failure has no such proof and requires final owner teardown.
        [[nodiscard]] RenderResult<std::optional<SubmissionTicket>> lastUse() const noexcept;
        [[nodiscard]] NativeGraphStatistics statistics() const noexcept;
        [[nodiscard]] std::string diagnostics() const noexcept;
        [[nodiscard]] std::size_t traceCapacity() const noexcept;
        [[nodiscard]] std::span<const NativeViewRequirement> importViews(GraphResourceId resource) const noexcept;

        [[nodiscard]] RenderResult<GraphSubmission> submit(
            const FrameGraphBindings& bindings,
            std::span<const NativeImportBinding> imports,
            std::span<NativeTraceEvent> trace = {}
        ) noexcept;

        [[nodiscard]] RenderResult<bool> completed(GraphSubmission receipt) noexcept;
        [[nodiscard]] RenderResult<void> readback(
            GraphSubmission receipt,
            GraphResourceId resource,
            VkDeviceSize offset,
            std::span<std::byte> destination
        ) noexcept;
        [[nodiscard]] RenderResult<void> readbackPass(
            GraphSubmission receipt,
            GraphPassId pass,
            std::uint32_t input,
            VkDeviceSize offset,
            std::span<std::byte> destination
        ) noexcept;
        // Explicit synchronous borrows; returned backing remains owned by this
        // plan (or its import owner). External GPU use must be returned through
        // retainExternalUse before slot reuse or retirement. No ownership transfer.
        [[nodiscard]] RenderResult<VNativeImport> exportedBacking(GraphSubmission receipt, GraphResourceId resource)
            const noexcept;
        [[nodiscard]] RenderResult<std::uint32_t> resourceStates(
            GraphSubmission receipt,
            GraphResourceId resource,
            std::span<NativeRangeState> destination
        ) const noexcept;
        [[nodiscard]] RenderResult<GraphSubmission> retainExternalUse(
            GraphSubmission receipt,
            SubmissionTicket last_use
        ) noexcept;

    private:
        friend RenderResult<ExecutableGraphPlan>
        compileVulkanGraph(const NativeCompileInputs&, std::vector<NativeShaderProgram>) noexcept;
        explicit ExecutableGraphPlan(std::unique_ptr<detail::NativeGraphBacking> backing) noexcept;
        std::unique_ptr<detail::NativeGraphBacking> backing_;
    };
} // namespace lux::render::vulkan
