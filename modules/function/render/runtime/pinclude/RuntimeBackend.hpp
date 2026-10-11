#pragma once

#include "Presentation.hpp"
#include <lux/engine/render/runtime/NativeAssembly.hpp>
#include <lux/engine/render/vulkan/retirement/Retirement.hpp>

namespace lux::render::detail
{
    struct ScenePassValue
    {
        RuntimePathId path;
        GraphPassId pass;
        GraphPassInvocation value;
    };

    struct RuntimeSceneState
    {
        std::uint64_t revision{};
        std::vector<ScenePassValue> passes;
    };

    struct RuntimeTargetLayer
    {
        RenderViewHandle view;
        RuntimeOutputPlacement placement;
    };

    struct RuntimeTargetState
    {
        std::uint32_t width, height;
        std::uint64_t epoch{1};
        std::unique_ptr<PresentationTarget> presentation;
        std::optional<RenderError> notice;
        std::vector<RuntimeTargetLayer> layers;
    };

    struct RuntimeViewState
    {
        RenderViewHandle identity;
        RenderSceneId scene;
        RenderTargetId target;
        RuntimePathId path;
        std::uint32_t variant;
        GraphInvocationData values;
        std::vector<GraphImportBinding> logical_imports;
        std::vector<vulkan::NativeImportBinding> native_imports;
        std::vector<std::uint32_t> import_assets;
        std::vector<std::optional<vulkan::GraphSubmission>> receipts;
        bool active{true};
        std::uint64_t publication_revision{};
        std::optional<vulkan::GraphSubmission> latest;
        std::uint64_t latest_frame{};
    };

    struct RuntimePathState
    {
        vulkan::ExecutableGraphPlan code;
        std::vector<vulkan::NativePassCommand> commands;
        GraphInvocationData initial_values;
        std::vector<vulkan::NativeImportBinding> imports;
        std::optional<GraphResourceId> output;
        std::vector<RuntimeFrameField> frame_fields;
    };

    struct RuntimeNativeVariant
    {
        struct TargetBacking
        {
            GraphResourceId resource;
            GraphBackingId identity;
            vulkan::Image image;
            std::vector<vulkan::ImageView> views;
            std::vector<std::reference_wrapper<const vulkan::ImageView>> view_borrows;
            std::vector<vulkan::NativeRangeState> states;
            std::uint32_t state_count{};
        };

        struct InvocationTargets
        {
            std::vector<std::unique_ptr<TargetBacking>> images;
            std::vector<vulkan::NativeImportBinding> imports;
        };

        RuntimePathId path;
        VkExtent2D extent;
        vulkan::ExecutableGraphPlan code;
        std::vector<InvocationTargets> targets;
        vulkan::NativeGraphInvocations invocations;
        std::vector<vulkan::NativeTraceEvent> trace;

        [[nodiscard]] VkDevice device() const noexcept
        {
            return code.device();
        }
    };

    using VRuntimeAsset = std::variant<vulkan::Buffer, vulkan::Image>;

    struct RuntimeAssetBacking
    {
        VRuntimeAsset backing;
        // Native state evidence belongs to this same backing owner. No second
        // asset registry, DataSet, CPU revision or speculative completion fact.
        std::vector<vulkan::NativeRangeState> states;
        std::uint32_t state_count{};
    };

    // One native owner tree. Destruction drains accepted work before any member
    // can destroy backing. SlotMap is only Core's generational CPU indexing.
    struct RuntimeBackendState
    {
        vulkan::VulkanInstance instance;
        vulkan::VulkanDevice device;
        vulkan::VulkanAllocator allocator;
        std::array<std::unique_ptr<vulkan::SubmissionQueue>, 3> queue_owners;
        std::array<vulkan::SubmissionQueue*, 3> queues;
        std::vector<RuntimeAssetBacking> assets;
        std::vector<vulkan::Sampler> samplers;
        std::vector<RuntimePathState> paths;
        vulkan::RetirementQueue retirement;
        std::vector<std::unique_ptr<RuntimeNativeVariant>> variants;
        cxx::SlotMap<RuntimeSceneState, RenderSceneTag> scenes;
        cxx::SlotMap<RuntimeTargetState, RenderTargetTag> targets;
        cxx::SlotMap<RuntimeViewState, RenderViewTag> views;
        RuntimeCapacity capacity;
        std::uint64_t owner;
        RuntimeProgress progress;
        std::optional<RenderError> terminal;
        std::uint64_t next_backing{1ull << 63};
        std::vector<PresentationLayer> output_layers;
        std::vector<RuntimeViewState*> output_views;

        RuntimeBackendState(
            vulkan::VulkanInstance host,
            vulkan::VulkanDevice native,
            vulkan::VulkanAllocator memory,
            std::array<std::unique_ptr<vulkan::SubmissionQueue>, 3> queue_storage,
            std::array<vulkan::SubmissionQueue*, 3> native_queues,
            vulkan::RetirementQueue retired,
            RuntimeCapacity limits,
            std::uint64_t identity
        ) noexcept;
        ~RuntimeBackendState() noexcept;
    };

    [[nodiscard]] RenderResult<std::uint32_t> findRuntimeVariant(
        RuntimeBackendState& backend,
        RuntimePathId path,
        VkExtent2D extent,
        std::span<const std::uint32_t> candidate_pins = {}
    ) noexcept;

    [[nodiscard]] RenderResult<void> drainRuntimeNative(RuntimeBackendState& backend) noexcept;
    [[nodiscard]] std::uint32_t runtimeAssetIndex(
        const RuntimeBackendState& backend,
        const vulkan::NativeImportBinding& input
    ) noexcept;

    [[nodiscard]] bool runtimeHasOutput(const RuntimeBackendState& backend) noexcept;
    [[nodiscard]] RenderResult<void> executeRuntimeFrame(
        RuntimeBackendState& backend,
        std::uint64_t serial,
        std::uint64_t time_ns
    ) noexcept;
    [[nodiscard]] RenderResult<void> applyRuntimeControl(RuntimeBackendState& backend, RuntimeControl request) noexcept;
    [[nodiscard]] RenderResult<void> applyRuntimePublication(
        RuntimeBackendState& backend,
        RuntimeScenePublication request,
        std::span<const std::byte> bytes
    ) noexcept;
    [[nodiscard]] RenderResult<void> applyRuntimePublication(
        RuntimeBackendState& backend,
        RuntimeViewPublication request,
        std::span<const std::byte> bytes
    ) noexcept;
} // namespace lux::render::detail
