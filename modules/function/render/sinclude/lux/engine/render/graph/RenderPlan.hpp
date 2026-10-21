#pragma once

#include <lux/engine/render/visibility.h>

#include <NRI.h>
#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/render/device/SubmissionQueue.hpp>
#include <lux/engine/render/graph/Diagnostics.hpp>
#include <lux/engine/render/graph/RenderGraphAnalysis.hpp>
#include <lux/engine/render/graph/Work.hpp>
#include <memory>

namespace lux::render::detail
{
    struct PipelineLayoutPlan;
    struct EnabledDeviceFeatures;
    class RenderPlan;
    [[nodiscard]] LUX_RENDER_PUBLIC bool dumpPlan(const RenderPlan&, EGraphDumpFormat, DumpSink) noexcept;

    struct CompiledResource final
    {
        std::string key;
        VGraphResourceDesc description;
        EGraphOrigin origin;
        nri::TextureUsageBits image_usage = nri::TextureUsageBits::NONE;
        nri::BufferUsageBits buffer_usage = nri::BufferUsageBits::NONE;
        bool used = false;
        bool memory_used = false;
        bool target_sized = false;
        bool shared_table = false;
        std::uint32_t history = kGraphInvalidIndex;
        // Immutable reverse indexes shared by every Context frame slice.
        std::vector<std::pair<std::uint32_t, std::uint32_t>> barrier_uses, descriptor_uses;
        std::vector<std::uint32_t> final_barrier_uses;
    };

    enum class ETransitionBoundary : std::uint8_t
    {
        PASS,
        RESTORE_IMPORTED,
        EXPORT
    };

    struct ImageTransition final
    {
        std::uint32_t resource;
        nri::TextureBarrierDesc native;
        GraphPassId previous;
        ETransitionBoundary boundary = ETransitionBoundary::PASS;
    };

    struct BufferTransition final
    {
        std::uint32_t resource;
        nri::BufferBarrierDesc native;
        GraphPassId previous;
        ETransitionBoundary boundary = ETransitionBoundary::PASS;
    };

    struct BarrierBatch final
    {
        std::vector<ImageTransition> images;
        std::vector<BufferTransition> buffers;
    };

    enum class ERenderingBoundary : std::uint8_t
    {
        NON_RASTER,
        FIRST_RASTER,
        ATTACHMENT_OR_LOAD_STORE,
        RESOURCE_DEPENDENCY,
        CONTINUATION
    };

    struct VertexSource final
    {
        GraphResourceId resource;
        std::uint32_t binding;
        std::uint64_t offset;
        std::uint32_t stride;
        bool operator==(const VertexSource&) const noexcept = default;
    };

    // Recording values contain neither author names, Shader artifacts nor pipeline construction state.
    struct RasterCommands final
    {
        std::vector<RasterAttachment> colors;
        std::optional<RasterAttachment> depth;
        std::vector<VertexSource> vertices;
        VDrawCommand draw;
        std::optional<RasterViewport> viewport;
        std::optional<RasterStateInput> state_input;
        std::array<float, 2> depth_bias{};
    };

    struct ComputeCommands final
    {
        VDispatchArguments dispatch;
    };

    using VPassCommands = std::variant<RasterCommands, ComputeCommands, TransferWork>;

    struct CompiledPass final
    {
        VPassCommands work;
        BarrierBatch before;
        nri::Pipeline* pipeline = nullptr;
        std::uint32_t source_pass = kGraphInvalidIndex;
        std::uint32_t rendering_group = kGraphInvalidIndex;
        ERenderingBoundary rendering_boundary = ERenderingBoundary::NON_RASTER;
        std::uint32_t condition_region = kGraphInvalidIndex;
        EQueueRole queue = EQueueRole::GRAPHICS;
    };

    // A linear, non-nested interval. Both paths execute its compiled transitions;
    // only the true path records work. The last transition establishes the common exit state.
    struct ConditionalRegion final
    {
        PassCondition predicate;
        std::uint32_t first_pass = 0;
        std::uint32_t pass_count = 1;
    };

    struct RenderingGroup final
    {
        std::uint32_t first_pass = 0;
        std::uint32_t pass_count = 1;
        std::vector<RasterAttachment> colors;
        std::optional<RasterAttachment> depth;
    };

    struct RecordingProgram final
    {
        std::vector<CompiledPass> passes;
        std::vector<RenderingGroup> groups;
        std::vector<ConditionalRegion> conditions;
        BarrierBatch after;
    };

    struct HistoryPlan final
    {
        HistoryDescription description;
        // Compiled producer indexes, including disjoint writes composing the published version.
        std::vector<std::uint32_t> writers;
    };

    struct SubmissionDependency final
    {
        std::uint32_t producer;
        nri::StageBits stages = nri::StageBits::ALL;
    };

    struct SubmissionBatch final
    {
        EQueueRole queue = EQueueRole::GRAPHICS;
        std::uint32_t first_pass = 0;
        std::uint32_t pass_count = 0;
        std::vector<SubmissionDependency> dependencies;
        std::vector<GraphResourceId> histories;
    };

    struct ImportedAccess final
    {
        GraphResourceId resource;
        VGraphRange range;
        bool write = false;
    };

    enum class ETransientAllocation : std::uint8_t
    {
        INDEPENDENT,
        REUSE_OBJECTS,
        ALIAS_MEMORY
    };

    enum class EResourceReuse : std::uint8_t
    {
        UNUSED,
        EXTERNAL,
        HISTORY,
        EXPORTED,
        CONDITIONAL,
        INITIAL_CONTENT,
        DISABLED,
        DISTINCT_DESCRIPTION,
        UNORDERED,
        REUSED
    };

    struct PhysicalResourceSlot final
    {
        GraphResourceId resource;
        std::array<std::uint32_t, 3> before{}, end{};
        std::array<std::uint32_t, 3> first_pass{kGraphInvalidIndex, kGraphInvalidIndex, kGraphInvalidIndex};
        std::array<nri::AccessBits, 3> accesses{};
    };

    // Logical bindings borrow these unique physical slots; the instance owns one set per FIF.
    struct PhysicalResourcePlan final
    {
        ETransientAllocation allocation = ETransientAllocation::REUSE_OBJECTS;
        std::vector<PhysicalResourceSlot> slots;
        std::vector<std::uint32_t> resource_slots;
        std::vector<EResourceReuse> reasons;
    };

    // The Effect owns this immutable result; execution contexts borrow it within the Effect lifetime.
    class LUX_RENDER_PUBLIC RenderPlan final
    {
    public:
        ~RenderPlan() noexcept;
        RenderPlan(RenderPlan&& other) noexcept;
        RenderPlan& operator=(RenderPlan&&) = delete;
        RenderPlan(const RenderPlan&) = delete;
        RenderPlan& operator=(const RenderPlan&) = delete;

        [[nodiscard]] nri::Device* device() const noexcept
        {
            return device_;
        }

        [[nodiscard]] const TargetDesc& target() const noexcept
        {
            return target_;
        }

        [[nodiscard]] std::span<const CompiledResource> resources() const noexcept
        {
            return resources_;
        }

        [[nodiscard]] std::span<const CompiledPass> passes() const noexcept
        {
            return recording_.passes;
        }

        [[nodiscard]] std::uint32_t output() const noexcept
        {
            return output_;
        }

        [[nodiscard]] bool compatible(const TargetDesc& target) const noexcept;

        [[nodiscard]] std::span<const SubmissionBatch> submissions() const noexcept
        {
            return submissions_;
        }

        [[nodiscard]] const PhysicalResourcePlan& physicalResources() const noexcept
        {
            return physical_resources_;
        }

    private:
        friend class RenderGraphInstance;
        friend class GraphCompiler;
        friend class RenderExecutor;
        friend class FrameExecution;
        friend LUX_RENDER_PUBLIC bool dumpPlan(const RenderPlan&, EGraphDumpFormat, DumpSink) noexcept;

        RenderPlan(nri::Device& device, const nri::CoreInterface&, TargetDesc target) noexcept;
        std::unique_ptr<PipelineLayoutPlan> descriptor_plan_;
        nri::Device* device_;
        const nri::CoreInterface* nri_core_;
        TargetDesc target_;
        std::vector<nri::PipelineLayout*> layouts_;
        std::vector<CompiledResource> resources_;
        std::vector<HistoryPlan> histories_;
        std::uint32_t condition_count_ = 0;
        RecordingProgram recording_;
        std::vector<SubmissionBatch> submissions_;
        std::vector<ImportedAccess> imported_accesses_;
        PhysicalResourcePlan physical_resources_;
        std::vector<std::uint32_t> pass_indices_;

        std::vector<nri::Pipeline*> pipelines_; // unique physical owners; recorded work only borrows handles
        std::uint32_t output_ = kGraphInvalidIndex;
        bool resizable_output_ = false;
    };

} // namespace lux::render::detail
