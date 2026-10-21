#pragma once

#include <lux/engine/render/visibility.h>

#include <NRI.h>
#include <lux/engine/render/core/Target.hpp>
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
        BarrierBatch after;
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

    private:
        friend class RenderGraphInstance;
        friend class GraphCompiler;
        friend class RenderExecutor;
        friend LUX_RENDER_PUBLIC bool dumpPlan(const RenderPlan&, EGraphDumpFormat, DumpSink) noexcept;

        RenderPlan(nri::Device& device, const nri::CoreInterface&, TargetDesc target) noexcept;
        std::unique_ptr<PipelineLayoutPlan> descriptor_plan_;
        nri::Device* device_;
        const nri::CoreInterface* nri_core_;
        TargetDesc target_;
        std::vector<nri::PipelineLayout*> layouts_;
        std::vector<CompiledResource> resources_;
        RecordingProgram recording_;
        std::vector<std::uint32_t> pass_indices_;

        std::vector<nri::Pipeline*> pipelines_; // unique physical owners; recorded work only borrows handles
        std::uint32_t output_ = kGraphInvalidIndex;
        bool resizable_output_ = false;
    };

} // namespace lux::render::detail
