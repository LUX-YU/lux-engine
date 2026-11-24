#pragma once

#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/graph/DrawList.hpp>
#include <lux/engine/render/graph/DrawResource.hpp>
#include <lux/engine/render/graph/IndexedGeometry.hpp>
#include <cstddef>
#include <cstdint>
#include <span>

namespace lux::render::runtime_detail { struct JobExecution; }

namespace lux::render::detail
{
    struct FrameRasterOutputs;
    struct DynamicBuffer;
    struct PreparedBufferInput;
    struct PreparedBufferReplica;
}

namespace lux::render
{
    class EffectFrameWriter;

    namespace extension_detail
    {
        template <class Fn> struct FrameSignature;
    }

    // Borrows this preparation and its selected Program until onFrame() returns.
    class LUX_RENDER_PUBLIC RasterRecordWriter final
    {
    public:
        [[nodiscard]] const RasterRecordLayout& layout() const & noexcept { return layout_; }
        const RasterRecordLayout& layout() const && = delete;

        [[nodiscard]] RenderResult<void>
        write(const RasterRecordInput&, std::optional<RasterResourceSlot> images = std::nullopt) const noexcept;

    private:
        friend class EffectFrameWriter;
        RasterRecordWriter(
            const detail::FrameRasterOutputs& draws, std::uint32_t list, std::uint32_t program, RasterRecordLayout layout
        ) noexcept : draws_(draws), list_(list), program_(program), layout_(layout) {}

        const detail::FrameRasterOutputs& draws_;
        std::uint32_t list_, program_;
        RasterRecordLayout layout_;
    };

    struct FrameBufferUpload final
    {
        std::span<const std::byte> bytes;
        std::uint64_t revision = 0;
    };

    // Borrows only this algorithm's output slots for the duration of its synchronous call.
    class LUX_RENDER_PUBLIC EffectFrameWriter final
    {
    public:
        EffectFrameWriter(const EffectFrameWriter&) = delete;
        EffectFrameWriter& operator=(const EffectFrameWriter&) = delete;

        // Publish all declared uploads and graph-written extents in their respective declaration order.
        [[nodiscard]] RenderResult<void>
        publishBuffers(std::span<const FrameBufferUpload> uploads, std::span<const std::uint64_t> extents) noexcept;

        [[nodiscard]] RenderResult<void> setRasterStream(DrawStreamSlot, const RasterStreamData&) noexcept;
        [[nodiscard]] RenderResult<RasterProgramSlot>
        rasterProgram(DrawStreamSlot, std::uint32_t selection_index) const noexcept;
        [[nodiscard]] RenderResult<RasterRecordWriter>
        rasterRecord(RasterResourceSlot source, RasterProgramSlot program) noexcept;

        [[nodiscard]] RenderResult<RasterIndexedGeometry>
        bindIndexedGeometry(RasterResourceSlot source, const IndexedGeometryInput& geometry) noexcept;

        [[nodiscard]] RenderResult<void>
        useSampledImage(RasterResourceSlot source, const nri::Texture& image, GraphImageRange range) noexcept;

    private:
        friend struct runtime_detail::JobExecution;
        template <class Fn> friend struct extension_detail::FrameSignature;
        EffectFrameWriter(
            const detail::FrameRasterOutputs& draws, std::span<const detail::DynamicBuffer> contracts,
            std::span<const detail::PreparedBufferReplica> replicas,
            std::span<detail::PreparedBufferInput> outputs
        ) noexcept : draws_(draws), contracts_(contracts), replicas_(replicas), outputs_(outputs) {}

        const detail::FrameRasterOutputs& draws_;
        std::span<const detail::DynamicBuffer> contracts_;
        std::span<const detail::PreparedBufferReplica> replicas_;
        std::span<detail::PreparedBufferInput> outputs_;
        std::span<std::byte> inline_results_;
        bool published_ = false;
    };

} // namespace lux::render
