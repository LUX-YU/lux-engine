#pragma once

#include <lux/engine/render/device/JobGpuResources.hpp>
#include <lux/engine/render/graph/execute/RasterStreamPlan.hpp>
#include <lux/engine/render/extensions/execution/ResourceInput.hpp>
#include <lux/engine/render/graph/DrawList.hpp>

namespace lux::render::detail
{
    struct OwnedRasterStream;
    struct FrameResources;
    struct RasterDescriptorLayout;

    // Synchronous preparation of a single writable FIF. Recording consumes only the published packet tables.
    class RasterFramePreparation final
    {
    public:
        RasterFramePreparation(
            const compiled::RenderGraph& graph, FrameResources& frame, const JobGpuResources& gpu
        ) noexcept
            : graph_(graph), frame_(frame), gpu_(gpu), session_(gpu.session) {}

        [[nodiscard]] RenderResult<void> prepareDescriptorCapacity(bool resize_pool) noexcept;
        [[nodiscard]] RenderResult<void> prepareSections() noexcept;
        [[nodiscard]] RenderResult<void> materializeStreams() noexcept;

    private:
        [[nodiscard]] RenderResult<ResourceInput>
        resolveBuffer(const CompiledRasterStream&, const RasterBufferSlice&, EGraphUse) const noexcept;
        [[nodiscard]] RenderResult<RasterDescriptorInput> resolveDescriptor(
            std::uint32_t pass, const CompiledRasterStream&, const RasterStreamBinding&, const RasterDescriptorField&
        ) const noexcept;
        [[nodiscard]] RenderResult<void> selectDrawBindings(
            std::uint32_t pass, const CompiledRasterStream&, const RasterStreamDraw&, const CompiledRasterStream::Program&
        ) noexcept;
        [[nodiscard]] RenderResult<void> appendDraw(
            std::uint32_t pass, const CompiledRasterStream&, const RasterStreamDraw&, const CompiledRasterStream::Program&
        ) noexcept;
        [[nodiscard]] RenderResult<void> materializeStream(std::uint32_t pass, const CompiledRasterStream&) noexcept;
        void publishStream(std::uint32_t pass, std::uint32_t list) noexcept;
        [[nodiscard]] RenderResult<void> resizeLayers(GraphResourceId, std::uint32_t layers) noexcept;
        [[nodiscard]] RenderResult<void> materializeSections(std::uint32_t pass, const OwnedRasterStream&) noexcept;
        [[nodiscard]] RenderResult<void> growDescriptorPool() noexcept;
        [[nodiscard]] const RasterDescriptorLayout& rasterBindings(RasterBindingKey) const noexcept;
        [[nodiscard]] RenderResult<nri::DescriptorSet*> descriptorSet(
            const RasterDescriptorLayout&, std::uint32_t record, std::span<const RasterDescriptorInput>
        ) noexcept;

        const compiled::RenderGraph& graph_;
        FrameResources& frame_;
        const JobGpuResources& gpu_;
        DeviceSession& session_;
    };
} // namespace lux::render::detail
