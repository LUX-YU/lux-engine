#pragma once

#include <lux/engine/render/vulkan/graph/Executable.hpp>

namespace lux::render::vulkan::detail
{
    using VResourceBacking = std::variant<
        std::monostate,
        Buffer,
        Image,
        std::reference_wrapper<const Buffer>,
        std::reference_wrapper<const Image>>;

    [[nodiscard]] const Buffer* bufferOf(const VResourceBacking& resource) noexcept;
    [[nodiscard]] const Image* imageOf(const VResourceBacking& resource) noexcept;

    // An atomic interval of native state: boundaries come from the already
    // validated logical uses. Splitting here plans barriers, not new versions.
    struct ResourceCell
    {
        GraphResourceId resource;
        VGraphRange range;
    };

    struct UseRecipe
    {
        std::uint32_t use_index;
        std::vector<std::uint32_t> cells;
        NativeResourceState destination;
        VkPipelineStageFlags2 alias_source_stages{};
    };

    struct AttachmentRecipe
    {
        std::uint32_t field;
        std::uint32_t view;
        VkRenderingAttachmentInfo native;
        std::optional<std::uint32_t> resolve_view;
    };

    using VCopyRegions = std::variant<VkBufferCopy, std::vector<VkBufferImageCopy>, std::vector<VkImageCopy>>;

    struct CopyRecipe
    {
        std::uint32_t source_use;
        std::optional<std::uint32_t> destination_use;
        VCopyRegions regions;
        VkDeviceSize bytes;
        std::uint32_t buffer_alignment{4};
    };

    struct NativePassRecipe
    {
        NativePassCommand command;
        std::optional<std::uint32_t> program;
        std::vector<UseRecipe> uses;
        std::vector<std::uint32_t> predecessors;
        std::vector<VkMemoryBarrier2> alias_barriers;
        std::vector<CopyRecipe> copies;
    };

    struct SlotPass
    {
        struct ViewPatch
        {
            GraphResourceId resource;
            std::optional<std::uint32_t> import_view;
        };

        struct DescriptorPatch
        {
            GraphPassId source_pass;
            std::uint32_t field;
            std::optional<std::uint32_t> use, primary_view, fallback_view;
        };

        std::vector<ImageView> views;
        std::vector<ViewPatch> view_patches;
        std::vector<const ImageView*> current_views;
        std::vector<DescriptorPatch> descriptor_patches;
        std::vector<VDescriptorValue> descriptor_values;
        std::optional<BoundDescriptorSets> descriptors;
        std::vector<AttachmentRecipe> colors;
        std::optional<AttachmentRecipe> depth, stencil;
        std::vector<VkRenderingAttachmentInfo> color_scratch;
        std::vector<std::uint32_t> dynamic_offsets;
        VkExtent2D extent{};
        std::vector<Buffer> readbacks;
    };

    struct GraphSlot
    {
        std::vector<VResourceBacking> resources;
        std::vector<SlotPass> passes;
        std::vector<NativeResourceState> states;
        std::vector<std::optional<SubmissionTicket>> pass_tickets;
        std::vector<std::optional<SubmissionTicket>> cell_tickets;
        std::array<std::optional<SubmissionTicket>, 3> queue_tickets;
        std::optional<SubmissionTicket> completion;
        std::uint64_t frame_serial{};
        bool external_use{};
        std::vector<VkDeviceSize> buffer_offsets;

        GraphSlot() = default;
        GraphSlot(GraphSlot&&) noexcept = default;
        GraphSlot& operator=(GraphSlot&&) = delete;
        GraphSlot(const GraphSlot&) = delete;

        ~GraphSlot() noexcept
        {
            passes.clear();
            while (!resources.empty())
            {
                resources.pop_back();
            }
        }
    };

    // Single composite owner. Destruction completes outstanding borrows before
    // slots (descriptors/views/backing), then programs (pipeline/layout) die.
    struct NativeGraphBacking
    {
        LogicalGraphPlan logical;
        VkDevice device;
        PFN_vkCmdSetRenderingInputAttachmentIndicesKHR set_input_indices{};
        std::array<SubmissionQueue*, 3> queues;
        std::vector<NativeShaderProgram> programs;
        std::vector<ResourceCell> cells;
        std::vector<NativePassRecipe> recipes;
        std::vector<NativeImportBinding> imports;
        std::vector<std::optional<std::uint32_t>> import_positions;
        std::vector<std::vector<NativeViewRequirement>> import_views;
        std::vector<std::pair<GraphSampler, std::reference_wrapper<const Sampler>>> samplers;
        std::vector<std::uint32_t> resource_usage;
        std::vector<std::optional<GraphResourceId>> alias_sources;
        std::vector<GraphSlot> slots;
        NativeGraphStatistics statistics;
        std::array<std::uint32_t, 3> admission{};
        std::optional<RenderError> terminal;

        NativeGraphBacking(const NativeCompileInputs& inputs, std::vector<NativeShaderProgram> programs) noexcept;
        ~NativeGraphBacking() noexcept;
    };

    [[nodiscard]] RenderResult<void> compileResources(
        NativeGraphBacking& result,
        const NativeCompileInputs& inputs
    ) noexcept;
    [[nodiscard]] RenderResult<void> compileRecipes(
        NativeGraphBacking& result,
        const NativeCompileInputs& inputs
    ) noexcept;
    [[nodiscard]] RenderResult<void> compileSync(NativeGraphBacking& result) noexcept;
    [[nodiscard]] RenderResult<void> compileCopies(NativePassRecipe& recipe, const LogicalGraphPlan& plan) noexcept;
    void recordCopies(
        VkCommandBuffer command,
        const NativePassRecipe& recipe,
        const GraphSlot& slot,
        const FrameGraphBindings& bindings
    ) noexcept;
    [[nodiscard]] RenderResult<void> prepareSlot(
        NativeGraphBacking& plan,
        GraphSlot& slot,
        const FrameGraphBindings& bindings,
        std::span<const NativeImportBinding> imports
    ) noexcept;
    [[nodiscard]] NativeResourceState useState(const GraphResourceUse& use, std::uint32_t family) noexcept;
    [[nodiscard]] VkImageSubresourceRange imageRange(const ImageRange& range) noexcept;
    [[nodiscard]] bool containsRange(const VGraphRange& parent, const VGraphRange& child) noexcept;
    [[nodiscard]] NativeRangeState importState(const NativeImportBinding& input, const ResourceCell& cell) noexcept;
    [[nodiscard]] bool matchesReceipt(const NativeGraphBacking& plan, GraphSubmission receipt) noexcept;
} // namespace lux::render::vulkan::detail
