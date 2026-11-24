#include <lux/engine/render/graph/execute/RasterDraws.hpp>
#include <lux/engine/render/effect/EffectFrameWriter.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/frame/FrameError.hpp>
#include <lux/engine/render/graph/Range.hpp>
#include <lux/engine/render/graph/Shader.hpp>
#include <lux/engine/render/runtime/JobExecution.hpp>
#include <lux/engine/render/runtime/commands/Error.hpp>
#include <Extensions/NRIHelper.h>
#include <algorithm>

namespace lux::render::detail
{
    namespace
    {
        RenderResult<RasterDrawInput::GeometryLayout> captureGeometryLayout(
            CpuDrawStorage& stream,
            const IndexedGeometryInput& input,
            const DeviceSession& device,
            std::uint32_t pass
        ) noexcept
        {
            const auto attributes = input.attributes;
            const auto& vertices = input.vertices;
            const auto stride = vertices.stride;
            const auto valid_stride = [&](std::uint32_t alignment) noexcept
            {
                return vertices.size == stride || stride % alignment == 0;
            };
            if (!stream.geometry.empty())
            {
                const auto& last = stream.geometry.back();
                const auto layout = last.layout;
                const auto range = layout.attributes;
                const auto alignment = layout.component_alignment;
                const auto& previous = last.binding.vertices;
                const auto prior = std::span{stream.geometry_attributes}.subspan(range.first, range.count);
                const bool same_origin = vertices.offset % alignment == previous.offset % alignment;
                if (same_origin && stride == previous.stride && std::ranges::equal(prior, attributes))
                {
                    if (!valid_stride(alignment)) return cxx::unexpected(RenderError{kGraphInvalidRange, {pass}});
                    return layout;
                }
            }
            const auto limit = device.description().shaderStage.vertex.attributeMaxNum;
            if (attributes.size() > limit) return cxx::unexpected(RenderError{kGraphInvalidRange, {pass}});
            std::uint32_t alignment = 1;
            for (std::size_t i = 0; i < attributes.size(); ++i)
            {
                const auto& field = attributes[i];
                const auto format = vertexFormat(field.format);
                const bool invalid_offset = field.offset > device.vertexOffsetLimit() ||
                    !validInterval(field.offset, format.bytes(), stride);
                const auto prior = attributes.first(i);
                const auto match = std::ranges::find(prior, field.location, &GeometryAttribute::location);
                const bool duplicate = match != prior.end();
                if (invalid_offset || field.location >= limit || duplicate)
                    return cxx::unexpected(RenderError{kGraphInvalidRange, {pass, i}});
                if ((vertices.offset + field.offset) % format.component_bytes)
                    return cxx::unexpected(RenderError{kGraphInvalidRange, {pass, i}});
                alignment = std::max(alignment, format.component_bytes);
            }
            if (!valid_stride(alignment)) return cxx::unexpected(RenderError{kGraphInvalidRange, {pass}});
            auto& selected = stream.geometry_attributes;
            const auto first = selected.size();
            if (attributes.size() > kGraphInvalidIndex - first)
                return cxx::unexpected(RenderError{kStorageCapacity, {pass}});
            selected.insert(selected.end(), attributes.begin(), attributes.end());
            return RasterDrawInput::GeometryLayout{
                {static_cast<std::uint32_t>(first), static_cast<std::uint32_t>(attributes.size())}, alignment
            };
        }
    }

    const CompiledRasterStream& CpuDrawStorage::compiledStream() const & noexcept
    {
        return *stream_;
    }

    RenderResult<CpuDrawStorage::ViewInput>
    CpuDrawStorage::viewInput(const InstalledDrawSources& sources, std::uint32_t argument, cxx::basic_type_info type) const noexcept
    {
        const auto& call = sources.draws_.plan.calls[sources.draws_.call];
        const auto types = call.call.methods->inputs;
        if (argument >= types.size()) return cxx::unexpected(RenderError{kStorageMismatch, {stream_->pass, argument}});
        const auto declared = types[argument];
        if (declared != type || declared.name() != type.name())
            return cxx::unexpected(RenderError{kStorageMismatch, {stream_->pass, argument}});
        const auto slot = call.first_view + argument;
        return ViewInput{job_->frame_resources[job_->view_sources[slot]].id, slot};
    }

    RenderResult<void> CpuDrawStorage::bindProgramInput(ViewInput input, ReadRasterPrograms read) noexcept
    {
        const auto& stream = compiledStream();
        if (!stream.runtime) return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {stream_->pass}});
        job_->program_bindings_.push_back({&stream, std::move(read), input.slot});
        return {};
    }

    RenderResult<RasterProgramSelection> CpuDrawStorage::programSlots() const noexcept
    {
        return job_->gpu_.programSlots(compiledStream());
    }

    RenderResult<RasterProgramSlot> CpuDrawStorage::programAt(std::uint32_t index) const noexcept
    {
        const auto& stream = *stream_;
        if (!stream.runtime) return cxx::unexpected(RenderError{kGraphUnknownPass, {stream_->pass}});
        const auto selected = job_->gpu_.programSlots(stream);
        if (!selected || index >= selected->size())
            return cxx::unexpected(RenderError{kFrameContract, {stream_->pass, index}});
        return RasterProgramSlot{stream_->pass, index, selected->revision};
    }

    RenderResult<RasterRecordLayout> CpuDrawStorage::recordLayout(RasterProgramSlot program) const noexcept
    {
        if (frame_slot == kGraphInvalidIndex) return cxx::unexpected(RenderError{kFrameContract, {stream_->pass}});
        auto selection = programSlots();
        if (!selection) return cxx::unexpected(selection.error());
        const bool valid_program = program.stream == stream_->pass && program.index < selection->size() &&
            program.revision == selection->revision && (*selection)[program.index].record_stages;
        if (!valid_program) return cxx::unexpected(RenderError{kFrameContract, {stream_->pass, program.index}});
        const auto& selected = (*selection)[program.index];
        return RasterRecordLayout{selected.record_layout, frame_slot, selected.shared_texture_stages != 0};
    }

    RenderResult<void> CpuDrawStorage::appendRecord(
        std::uint32_t program, const RasterRecordLayout& layout, const RasterRecordInput& input
    ) noexcept
    {
        const auto stride = layout.block.size;
        if (!input.buffer || !stride) return cxx::unexpected(RenderError{kGraphInvalidRange, {stream_->pass, program}});
        const auto& desc = job_->gpu_.session.nriCore().GetBufferDesc(*input.buffer);
        const bool valid_buffer = (desc.usage & nri::BufferUsageBits::SHADER_RESOURCE) != 0 &&
            desc.structureStride == stride && input.range.offset % stride == 0 && input.range.size == stride &&
            validInterval(input.range.offset, input.range.size, desc.size);
        if (!valid_buffer) return cxx::unexpected(RenderError{kGraphInvalidRange, {stream_->pass, program}});
        if (!layout.reads_images && !input.images.empty()) return cxx::unexpected(RenderError{kStorageMismatch, {stream_->pass, program}});
        for (const auto& image : input.images)
            if (!validSampledImage(image)) return cxx::unexpected(RenderError{kGraphInvalidRange, {stream_->pass, program}});
        if constexpr (kCollectReadMetrics)
        {
            const auto first = record_images.size();
            if (input.images.size() > kGraphInvalidIndex - first)
                return cxx::unexpected(RenderError{kDeviceCapacity, {stream_->pass}});
            record_images.insert(record_images.end(), input.images.begin(), input.images.end());
            records.push_back({
                input.buffer, input.range, program, static_cast<std::uint32_t>(first),
                static_cast<std::uint32_t>(input.images.size())
            });
        }
        return {};
    }

    RenderResult<void> CpuDrawStorage::qualifyGeometrySlices(const IndexedGeometryInput& input) const noexcept
    {
        const auto pass = stream_->pass;
        const auto& device = job_->gpu_.session;
        const auto valid_slice = [&](const GeometryBufferSlice& slice, nri::BufferUsageBits usage) noexcept
        {
            if (!slice.buffer || !slice.stride) return false;
            const auto& desc = device.nriCore().GetBufferDesc(*slice.buffer);
            return (desc.usage & usage) != 0 && validInterval(slice.offset, slice.size, desc.size) &&
                slice.size % slice.stride == 0;
        };
        const auto& vertices = input.vertices;
        const auto& indices = input.indices;
        const bool valid_vertices = valid_slice(vertices, nri::BufferUsageBits::VERTEX) &&
            vertices.stride <= device.vertexStrideLimit();
        const bool valid_indices = (indices.stride == 2 || indices.stride == 4) &&
            valid_slice(indices, nri::BufferUsageBits::INDEX) && indices.offset % indices.stride == 0;
        if (!valid_vertices || !valid_indices) return cxx::unexpected(RenderError{kGraphInvalidRange, {pass}});
        return {};
    }

    void CpuDrawStorage::beginGeometrySelection() noexcept
    {
        if (selection == job_->draw_selection_) return;
        selection = job_->draw_selection_;
        geometry.clear();
        geometry_attributes.clear();
    }

    void CpuDrawStorage::markCommandsChanged() noexcept
    {
        command_revision = job_->draw_selection_;
    }

    RasterIndexedGeometry CpuDrawStorage::indexedGeometry(std::uint32_t row) const noexcept
    {
        const auto& binding = geometry[row].binding;
        const auto& vertices = binding.vertices;
        const auto& indices = binding.indices;
        const auto first = row * 2u;
        const auto pass = stream_->pass;
        return {
            {{first, pass, selection}, 0, vertices.size},
            {{first + 1, pass, selection}, 0, indices.size},
            indices.stride == 2 ? EIndexType::UINT16 : EIndexType::UINT32,
            static_cast<std::uint32_t>(indices.offset / indices.stride),
            static_cast<std::int32_t>(vertices.offset / vertices.stride)
        };
    }

    RenderResult<RasterIndexedGeometry>
    CpuDrawStorage::appendGeometry(ResourceInstanceId owner, const IndexedGeometryInput& input) noexcept
    {
        beginGeometrySelection();
        const auto pass = stream_->pass;
        const auto& vertices = input.vertices;
        const auto& indices = input.indices;
        const auto first_index = indices.offset / indices.stride;
        const auto vertex_offset = vertices.offset / vertices.stride;
        const bool exhausted = geometry.size() >= kGraphInvalidIndex / 2;
        if (exhausted || first_index > UINT32_MAX || vertex_offset > INT32_MAX)
            return cxx::unexpected(RenderError{kStorageCapacity, {pass}});

        const auto row = static_cast<std::uint32_t>(geometry.size());
        const auto layout = captureGeometryLayout(*this, input, job_->gpu_.session, pass);
        if (!layout) return cxx::unexpected(layout.error());
        geometry.push_back({{owner, input.item, vertices, indices}, *layout});
        return indexedGeometry(row);
    }

    bool CpuDrawStorage::validSampledImage(const RasterSampledImageRead& read) const noexcept
    {
        if (!read.image) return false;
        const auto& range = read.range;
        const auto& desc = job_->gpu_.session.nriCore().GetTextureDesc(*read.image);
        const bool valid_mips = validInterval(range.first_mip, range.mip_count, desc.mipNum);
        const bool valid_layers = validInterval(range.first_layer, range.layer_count, desc.layerNum);
        const bool sampled = (desc.usage & nri::TextureUsageBits::SHADER_RESOURCE) != 0;
        const bool valid_image = sampled && desc.type == nri::TextureType::TEXTURE_2D && desc.sampleNum == 1 &&
            !nri::nriGetFormatProps(desc.format)->isDepth;
        return valid_image && range.aspects == 1 && valid_mips && valid_layers;
    }
} // namespace lux::render::detail
