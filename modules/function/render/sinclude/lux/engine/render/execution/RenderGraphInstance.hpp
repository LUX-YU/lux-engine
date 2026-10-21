#pragma once

#include <lux/engine/render/visibility.h>

#include <lux/engine/render/core/Execution.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/core/RasterViewport.hpp>
#include <lux/engine/render/execution/ResourceInput.hpp>
#include <lux/engine/render/graph/RenderPlan.hpp>

namespace lux::render::detail
{
    class DeviceSession;
    class OffscreenTarget;
    class FrameExecution;

    class LUX_RENDER_PUBLIC RenderGraphInstance final
    {
    public:
        RenderGraphInstance(
            DeviceSession&,
            std::uint32_t frame_count,
            const RenderPlan&,
            OffscreenTarget* = nullptr,
            std::uint32_t parameter_bytes = 0
        ) noexcept;
        [[nodiscard]] RenderResult<void> initialize() noexcept;
        // Borrowed physical inputs are installed before initialize; the caller retains them through GPU completion.
        void bindImage(GraphResourceId resource, std::uint32_t slot, nri::Texture& image) noexcept;
        [[nodiscard]] RenderResult<void> bindSampler(
            std::uint32_t input,
            nri::Descriptor&,
            bool comparison = false
        ) noexcept;

        [[nodiscard]] std::size_t samplerInputCount() const noexcept
        {
            return samplers_.size();
        }

        [[nodiscard]] bool usesSampler(std::uint32_t input) const noexcept
        {
            return samplers_[input].used;
        }

        ~RenderGraphInstance() noexcept;
        RenderGraphInstance(const RenderGraphInstance&) = delete;
        RenderGraphInstance& operator=(const RenderGraphInstance&) = delete;

        [[nodiscard]] nri::Device* device() const noexcept
        {
            return plan_->device();
        }

        [[nodiscard]] const RenderPlan& plan() const noexcept
        {
            return *plan_;
        }

        [[nodiscard]] const TargetDesc& target() const noexcept
        {
            return target_;
        }

        [[nodiscard]] RenderResult<void> setInstanceParameters(std::span<const std::byte>) noexcept;
        [[nodiscard]] RenderResult<void> setConditions(std::span<const std::uint8_t>) noexcept;
        [[nodiscard]] RenderResult<void> setInputs(std::span<const std::byte>, std::span<const std::uint8_t>) noexcept;

        [[nodiscard]] std::span<const std::uint8_t> conditions() const noexcept
        {
            return condition_values_;
        }

        // An empty selection invalidates every declared history at the next preparation boundary.
        [[nodiscard]] RenderResult<void> invalidateHistory(std::span<const GraphResourceId> previous = {}) noexcept;

        [[nodiscard]] std::span<const std::byte> instanceParameters() const noexcept
        {
            return instance_values_;
        }

        [[nodiscard]] RenderResult<void> setParameters(GraphPassId, std::span<const std::byte>) noexcept;
        [[nodiscard]] std::span<const std::byte> parameters(GraphPassId) const noexcept;

        void setViewport(const RasterViewport& viewport) noexcept
        {
            viewport_ = viewport;
        }

        [[nodiscard]] const RasterViewport& viewport() const noexcept
        {
            return viewport_;
        }

        // FrameExecution selects the reusable slot and owns the recording/submission segment.
        [[nodiscard]] RenderResult<bool> prepareFrame(std::uint32_t slot) noexcept;

        [[nodiscard]] const ExecutionProgress& progress() const noexcept
        {
            return progress_;
        }

        // Trusted compiled resource index and Session slot, not a business resource lookup.
        void setResource(std::uint32_t resource, std::uint32_t slot, const ResourceInput&) noexcept;
        [[nodiscard]] ResourceInput resourceInput(GraphResourceId, std::uint32_t slot) const noexcept;
        // Cold diagnostics: sum actual NRI allocation requirements for this instance's owned resources.
        [[nodiscard]] bool dumpResources(DumpSink) const noexcept;
        [[nodiscard]] nri::Buffer* nativeBuffer(std::uint32_t resource, std::uint32_t slot) const noexcept;

    private:
        friend class FrameExecution;
        friend class RenderExecutor;
        void submitted(std::uint32_t slot) noexcept;
        void acceptedBatch(std::uint32_t batch, std::size_t queue, std::uint64_t serial) noexcept;
        void failedSubmission(RenderError error) noexcept;
        [[nodiscard]] std::uint32_t recordedPassCount(std::uint32_t slot) const noexcept;
        [[nodiscard]] RenderResult<void> createFrameResources(std::uint32_t slot) noexcept;
        [[nodiscard]] RenderResult<void> prepareFrameBindings(std::uint32_t slot) noexcept;
        [[nodiscard]] RenderResult<void> prepareDescriptors() noexcept;
        [[nodiscard]] RenderResult<void> rebind(std::uint32_t slot) noexcept;

        struct Binding final
        {
            nri::Texture* image = nullptr;
            nri::Descriptor* view = nullptr;
            nri::Buffer* buffer = nullptr;
            std::uint64_t buffer_offset = 0, buffer_size = 0;
            std::span<const std::byte> parameters;
            const rdesc::ShaderBlock* parameter_layout = nullptr;
            bool dirty = false;
        };

        struct HistoryState;
        struct FrameResources;
        [[nodiscard]] RenderResult<nri::TextureDesc> textureDescription(const CompiledResource&) const noexcept;
        [[nodiscard]] RenderResult<void> allocatePhysicalResources(FrameResources&) noexcept;
        void prepareAliasBarriers(FrameResources&) noexcept;
        [[nodiscard]] RenderResult<void> createResource(std::uint32_t resource, Binding&) noexcept;
        [[nodiscard]] RenderResult<void> createHistories() noexcept;
        [[nodiscard]] RenderResult<void> createHistoryViews(HistoryState&) noexcept;
        [[nodiscard]] bool selectHistory() noexcept;
        void bindHistory(std::uint32_t slot) noexcept;
        void updateHistoryViews(std::uint32_t slot) noexcept;
        void publishHistory(std::uint32_t slot) noexcept;

        struct Pass final
        {
            const CompiledPass* compiled_pass;
            std::vector<nri::TextureBarrierDesc> images;
            std::vector<nri::BufferBarrierDesc> buffers;
            std::optional<nri::GlobalBarrierDesc> alias_barrier;
            std::vector<nri::DescriptorSet*> descriptors;
            nri::DescriptorSet* private_descriptors = nullptr;
            std::vector<nri::Descriptor*> private_views;
            std::vector<nri::Descriptor*> history_views;
            std::vector<nri::AttachmentDesc> colors;
            nri::AttachmentDesc depth{};
            std::vector<nri::TextureDataLayoutDesc> copy_layouts;
            std::vector<nri::Descriptor*> clear_views;
            std::array<std::uint32_t, 3> dispatch_groups{};
            std::vector<std::byte> parameters;
            RasterState raster_state;
        };

        struct ParameterCopy final
        {
            std::uint32_t resource;
            std::uint32_t source_offset, destination_offset, bytes;
        };

        DeviceSession* session_;
        std::vector<FrameResources> frames_;
        std::vector<HistoryState> histories_;
        const RenderPlan* plan_;
        TargetDesc target_;
        RasterViewport viewport_;
        OffscreenTarget* output_;

        struct SamplerInput final
        {
            nri::Descriptor* descriptor = nullptr;
            bool comparison = false;
            bool used = false;
        };

        std::vector<SamplerInput> samplers_;
        std::vector<std::vector<std::byte>> pass_parameters_;
        std::vector<std::vector<ParameterCopy>> parameter_copies_;
        std::vector<std::byte> instance_values_;
        std::vector<std::uint8_t> condition_values_;
        ExecutionProgress progress_;
        RenderError submission_error_;
    };
} // namespace lux::render::detail
