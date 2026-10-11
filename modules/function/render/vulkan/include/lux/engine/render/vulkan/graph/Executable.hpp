#pragma once

#include <lux/engine/render/graph/Bindings.hpp>
#include <lux/engine/render/vulkan/shader/Bindings.hpp>
#include <lux/engine/render/vulkan/transfer/Submission.hpp>

namespace lux::render::vulkan
{
    namespace detail
    {
        struct NativeGraphBacking;
        struct NativeGraphCode;
    } // namespace detail

    struct DispatchCommand
    {
        std::uint32_t x{1}, y{1}, z{1};
        bool operator==(const DispatchCommand&) const noexcept = default;
    };

    // References a pass's already validated ResourceUse. Offset is relative to
    // that use's declared byte range; stride remains the pipeline's sole fact.
    struct VertexStream
    {
        std::uint32_t binding{}, use_index{};
        VkDeviceSize offset{};
        bool operator==(const VertexStream&) const noexcept = default;
    };

    struct IndexStream
    {
        std::uint32_t use_index{};
        VkIndexType type{VK_INDEX_TYPE_UINT32};
        VkDeviceSize offset{};
        bool operator==(const IndexStream&) const noexcept = default;
    };

    struct DrawCommand
    {
        std::uint32_t vertices{3}, instances{1}, first_vertex{}, first_instance{};
        std::vector<VertexStream> streams;
        bool operator==(const DrawCommand&) const noexcept = default;
    };

    struct DrawIndexedCommand
    {
        IndexStream indices;
        std::uint32_t indices_count{3}, instances{1}, first_index{};
        std::int32_t vertex_offset{};
        std::uint32_t first_instance{};
        std::vector<VertexStream> streams;
        bool operator==(const DrawIndexedCommand&) const noexcept = default;
    };

    // Exactly one command. GPU producers must bound command contents to the
    // declared geometry ranges and use firstInstance=0 (feature not enabled).
    // Admission validates storage/offset/stride; it never reads GPU arguments.
    struct IndirectArguments
    {
        std::uint32_t use_index{};
        VkDeviceSize offset{};
        std::uint32_t stride{sizeof(VkDrawIndirectCommand)}, count{1};
        bool operator==(const IndirectArguments&) const noexcept = default;
    };

    struct DrawIndirectCommand
    {
        IndirectArguments arguments;
        std::vector<VertexStream> streams;
        bool operator==(const DrawIndirectCommand&) const noexcept = default;
    };

    struct DrawIndexedIndirectCommand
    {
        IndexStream indices;
        IndirectArguments arguments{0, 0, sizeof(VkDrawIndexedIndirectCommand), 1};
        std::vector<VertexStream> streams;
        bool operator==(const DrawIndexedIndirectCommand&) const noexcept = default;
    };

    // Typed resource uses already specify source/destination and exact ranges.
    // Copy has no second list of resource identities to reconcile.
    struct CopyCommand
    {
        bool operator==(const CopyCommand&) const noexcept = default;
    };

    using VNativeCommand = std::variant<
        DispatchCommand,
        DrawCommand,
        DrawIndexedCommand,
        DrawIndirectCommand,
        DrawIndexedIndirectCommand,
        CopyCommand>;

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

    // Structural refinement for composition of imported Buffer state cells.
    // It carries no runtime backing identity and owns no resource.
    struct NativeBufferBoundary
    {
        GraphResourceId resource;
        std::uint64_t byte_offset;
    };

    struct NativeCompileInputs
    {
        const LogicalGraphPlan& logical;
        const VulkanDevice& device;
        std::array<SubmissionQueue*, 3> queues;
        std::span<const NativePassCommand> commands;
        bool alias{true};
        // Physical 1:1 TARGET_RELATIVE extent; zero is only legal for absolute graphs.
        VkExtent2D target_extent{};
        std::span<const NativeBufferBoundary> buffer_boundaries{};
    };

    // Cold allocation inputs for a fixed, bounded number of independent
    // invocations. Capacity is not frames-in-flight and is never inferred from
    // GraphFrameValues. Native borrows must outlive their invocation use.
    struct NativeInvocationInputs
    {
        const VulkanDevice& device;
        const VulkanAllocator& allocator;
        std::span<const NativeImportBinding> imports;
        const GraphInvocationData& initial_values;
        std::span<const std::pair<GraphSampler, std::reference_wrapper<const Sampler>>> samplers{};
        std::uint32_t capacity{2};
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
        REUSE_SCENE,
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

    // Cold physical specialization, retaining the exact immutable Shader/PSO/Layout
    // owners from compatible code. Uses the same compiler and full identity checks.
    [[nodiscard]] RenderResult<ExecutableGraphPlan> specializeVulkanGraph(
        const NativeCompileInputs& inputs,
        const ExecutableGraphPlan& shaders
    ) noexcept;

    // Immutable native code. Invocation owners pin this code once at cold
    // construction; submission does not copy pipelines or churn shared owners.
    class ExecutableGraphPlan
    {
    public:
        ~ExecutableGraphPlan() noexcept;
        ExecutableGraphPlan(ExecutableGraphPlan&&) noexcept;
        ExecutableGraphPlan& operator=(ExecutableGraphPlan&&) noexcept;
        // Cold cache handles share immutable code; no native owner is copied.
        ExecutableGraphPlan(const ExecutableGraphPlan&) noexcept = default;
        ExecutableGraphPlan& operator=(const ExecutableGraphPlan&) noexcept = default;
        [[nodiscard]] const LogicalGraphPlan& logical() const noexcept;
        [[nodiscard]] VkDevice device() const noexcept;
        [[nodiscard]] VkExtent2D targetExtent() const noexcept;
        [[nodiscard]] std::span<const NativeShaderProgram> shaderPrograms() const noexcept;
        [[nodiscard]] RenderResult<std::uint64_t> resourceUsage(GraphResourceId resource) const noexcept;
        [[nodiscard]] RenderResult<std::uint32_t> resourceStateCount(GraphResourceId resource) const noexcept;
        [[nodiscard]] NativeGraphStatistics statistics() const noexcept;
        [[nodiscard]] std::span<const NativeViewRequirement> importViews(GraphResourceId resource) const noexcept;

    private:
        friend class NativeGraphInvocations;
        friend RenderResult<ExecutableGraphPlan>
        specializeVulkanGraph(const NativeCompileInputs&, const ExecutableGraphPlan&) noexcept;
        friend RenderResult<ExecutableGraphPlan>
        compileVulkanGraph(const NativeCompileInputs&, std::vector<NativeShaderProgram>) noexcept;
        ExecutableGraphPlan(std::shared_ptr<const detail::NativeGraphCode> code) noexcept;
        std::shared_ptr<const detail::NativeGraphCode> code_;
    };

    class NativeGraphInvocations
    {
    public:
        [[nodiscard]] static RenderResult<NativeGraphInvocations> create(
            const ExecutableGraphPlan& plan,
            const NativeInvocationInputs& inputs
        ) noexcept;
        ~NativeGraphInvocations() noexcept;
        NativeGraphInvocations(NativeGraphInvocations&&) noexcept;
        NativeGraphInvocations& operator=(NativeGraphInvocations&&) noexcept;
        NativeGraphInvocations(const NativeGraphInvocations&) = delete;
        NativeGraphInvocations& operator=(const NativeGraphInvocations&) = delete;

        [[nodiscard]] const LogicalGraphPlan& logical() const noexcept;
        [[nodiscard]] VkDevice device() const noexcept;
        // Latest completion join covers all successful invocations on all native queues.
        // A partial-submit failure has no such proof and requires final owner teardown.
        [[nodiscard]] RenderResult<std::optional<SubmissionTicket>> lastUse() const noexcept;
        [[nodiscard]] NativeGraphStatistics statistics() const noexcept;
        [[nodiscard]] std::string diagnostics() const noexcept;
        [[nodiscard]] std::size_t traceCapacity() const noexcept;
        // Owner-domain admission inspection. A Runtime can sum these bounds
        // across all planned invocations before beginning one frame transaction.
        [[nodiscard]] std::array<std::uint32_t, 3> submissionBudget() const noexcept;
        [[nodiscard]] RenderResult<bool> slotReady(std::uint32_t invocation_slot) noexcept;
        [[nodiscard]] std::span<const NativeViewRequirement> importViews(GraphResourceId resource) const noexcept;

        // Reuse requires an accepted same-frame donor in this owner. The owner
        // compares captured inputs with F2 scope proof, retains actual backing
        // through every consumer completion, and never treats revision as a fence.
        [[nodiscard]] RenderResult<GraphSubmission> submit(
            std::uint32_t invocation_slot,
            const FrameGraphBindings& bindings,
            std::span<const NativeImportBinding> imports,
            std::span<NativeTraceEvent> trace = {},
            std::optional<GraphSubmission> scene_source = {}
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
        explicit NativeGraphInvocations(std::unique_ptr<detail::NativeGraphBacking> backing) noexcept;
        std::unique_ptr<detail::NativeGraphBacking> backing_;
    };
} // namespace lux::render::vulkan
