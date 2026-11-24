#include <lux/engine/render/device/JobGpuResources.hpp>
#include <lux/engine/render/effect/EffectFrameWriter.hpp>
#include <lux/engine/render/runtime/InstalledFrameResource.hpp>
#include <lux/engine/render/resource/StorageMeta.hpp>
#include <lux/engine/render/frame/FrameError.hpp>
#include <lux/engine/render/graph/execute/RasterDraws.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/effect/LinkedFrameAlgorithms.hpp>

namespace lux::render
{
    RenderResult<std::uint32_t>
    detail::FrameAlgorithms::drawOutput(std::uint32_t call, std::uint32_t pass) const noexcept
    {
        const auto output = pass < output_by_pass.size() ? output_by_pass[pass] : kGraphInvalidIndex;
        if (!calls[call].streams.contains(output)) return cxx::unexpected(RenderError{kGraphUnknownPass, {pass}});
        return output;
    }

    RenderResult<std::uint32_t>
    detail::FrameAlgorithms::drawOutput(std::uint32_t call, RasterResourceSlot source) const noexcept
    {
        if (source.index >= draw_resource_bindings.size())
            return cxx::unexpected(RenderError{kGraphUnknownResource, {kGraphInvalidIndex, source.index}});
        const auto output = draw_resource_bindings[source.index].output;
        if (!calls[call].streams.contains(output))
            return cxx::unexpected(RenderError{kGraphUnknownPass, {draw_outputs[output]->pass}});
        return output;
    }

    RenderResult<std::uint32_t>
    detail::FrameRasterOutputs::outputFor(RasterResourceSlot source, EDrawResourceRead read) const noexcept
    {
        const auto output = plan.drawOutput(call, source);
        if (!output) return cxx::unexpected(output.error());
        const auto pass = plan.draw_outputs[*output]->pass;
        if (resources[source.index].isNull())
        {
            const auto error = read == EDrawResourceRead::SAMPLED_IMAGES ? kStorageMismatch : kStorageIdentity;
            return cxx::unexpected(RenderError{error, {pass, source.index}});
        }
        if (!plan.draw_resource_bindings[source.index].reads.containsAny(read))
            return cxx::unexpected(RenderError{kStorageMismatch, {pass, source.index}});
        return output;
    }

    RenderResult<InstalledDrawSources::Source<void>>
    InstalledDrawSources::resolve(RasterResourceSlot source, DrawResourceReads required, cxx::basic_type_info type) const noexcept
    {
        const auto list = draws_.plan.drawOutput(draws_.call, source);
        if (!list) return cxx::unexpected(list.error());
        const auto& binding = draws_.plan.draw_resource_bindings[source.index];
        const auto& resource = resources_[binding.binding];
        if (resource.factory)
        {
            const auto actual = resource.factory->storage().implementation_type;
            const bool wrong_type = actual != type || actual.name() != type.name();
            if (wrong_type || !binding.reads.containsAll(required))
                return cxx::unexpected(RenderError{kStorageMismatch, {draws_.plan.draw_outputs[*list]->pass, source.index}});
        }
        return Source<void>{resource.instance, draws_.streams[*list]};
    }

    RenderResult<void> EffectFrameWriter::setRasterStream(DrawStreamSlot slot, const RasterStreamData& data) noexcept
    {
        if (!data.fitsSlotRanges()) return cxx::unexpected(RenderError{kDeviceCapacity});
        auto list = draws_.plan.drawOutput(draws_.call, slot.index);
        if (!list) return cxx::unexpected(list.error());
        auto& stream = draws_.streams[*list];
        if (stream.written) return cxx::unexpected(RenderError{kFrameContract, {slot.index}});
        // A callback that binds no geometry this run cannot consume handles from its previous invocation.
        stream.beginGeometrySelection();
        stream.draws.assign(data.draws.begin(), data.draws.end());
        stream.bindings.assign(data.bindings.begin(), data.bindings.end());
        stream.vertices.assign(data.vertices.begin(), data.vertices.end());
        stream.constants.assign(data.constants.begin(), data.constants.end());
        stream.sections.assign(data.sections.begin(), data.sections.end());
        stream.written = true;
        return {};
    }

    RenderResult<RasterProgramSlot>
    EffectFrameWriter::rasterProgram(DrawStreamSlot slot, std::uint32_t selection_index) const noexcept
    {
        const auto grant = draws_.plan.drawOutput(draws_.call, slot.index);
        if (!grant) return cxx::unexpected(grant.error());
        return draws_.streams[*grant].programAt(selection_index);
    }

    RenderResult<RasterRecordWriter>
    EffectFrameWriter::rasterRecord(RasterResourceSlot source, RasterProgramSlot program) noexcept
    {
        const auto list = draws_.outputFor(source, EDrawResourceRead::STORAGE_RECORDS);
        if (!list) return cxx::unexpected(list.error());
        auto layout = draws_.streams[*list].recordLayout(program);
        if (!layout) return cxx::unexpected(layout.error());
        return RasterRecordWriter{draws_, *list, program.index, *layout};
    }

    RenderResult<void>
    RasterRecordWriter::write(const RasterRecordInput& input, std::optional<RasterResourceSlot> images) const noexcept
    {
        if (!input.images.empty())
        {
            const auto pass = draws_.plan.draw_outputs[list_]->pass;
            if (!images) return cxx::unexpected(RenderError{kStorageMismatch, {pass}});
            const auto list = draws_.outputFor(*images, EDrawResourceRead::SAMPLED_IMAGES);
            if (!list) return cxx::unexpected(list.error());
            if (*list != list_) return cxx::unexpected(RenderError{kStorageMismatch, {pass, images->index}});
        }
        return draws_.streams[list_].appendRecord(program_, layout_, input);
    }

    RenderResult<RasterIndexedGeometry>
    EffectFrameWriter::bindIndexedGeometry(RasterResourceSlot source, const IndexedGeometryInput& input) noexcept
    {
        const auto grant = draws_.outputFor(source, EDrawResourceRead::INDEXED_GEOMETRY);
        if (!grant) return cxx::unexpected(grant.error());
        auto& output = draws_.streams[*grant];
        if (auto valid = output.qualifyGeometrySlices(input); !valid) return cxx::unexpected(valid.error());
        return output.appendGeometry(draws_.resources[source.index], input);
    }

    RenderResult<void>
    EffectFrameWriter::useSampledImage(RasterResourceSlot source, const nri::Texture& image, GraphImageRange range) noexcept
    {
        const auto grant = draws_.outputFor(source, EDrawResourceRead::SAMPLED_IMAGES);
        if (!grant) return cxx::unexpected(grant.error());
        const RasterSampledImageRead read{&image, range};
        if (!draws_.streams[*grant].validSampledImage(read))
            return cxx::unexpected(RenderError{kGraphInvalidRange, {draws_.plan.draw_outputs[*grant]->pass, source.index}});
        if constexpr (detail::kCollectReadMetrics) draws_.streams[*grant].textures.push_back(read);
        return {};
    }

    RenderResult<void> EffectFrameWriter::publishBuffers(
        std::span<const FrameBufferUpload> uploads, std::span<const std::uint64_t> extents
    ) noexcept
    {
        if (published_) return cxx::unexpected(RenderError{kFrameContract});
        if (uploads.size() > contracts_.size() || extents.size() != contracts_.size() - uploads.size())
            return cxx::unexpected(RenderError{kGraphUnknownResource, {kGraphInvalidIndex, contracts_.size()}});

        for (std::size_t i = 0; i < contracts_.size(); ++i)
        {
            const auto& contract = contracts_[i];
            const bool is_upload = i < uploads.size();
            const auto size = is_upload ? uploads[i].bytes.size() : extents[i - uploads.size()];
            const bool invalid_size = size < contract.minimum_bytes || size > contract.maximum_bytes || size % 4;
            if (contract.upload != is_upload || invalid_size)
                return cxx::unexpected(RenderError{kFrameContract, {kGraphInvalidIndex, contract.resource}});
        }
        for (std::size_t i = 0; i < outputs_.size(); ++i)
        {
            auto& output = outputs_[i];
            output.bytes.clear();
            if (i < uploads.size())
            {
                const auto& upload = uploads[i];
                output.size = upload.bytes.size();
                output.revision = upload.revision;
                if (contracts_[i].used && !replicas_[i].contains(upload.revision, output.size))
                    output.bytes.assign(upload.bytes.begin(), upload.bytes.end());
            }
            else
            {
                output.size = extents[i - uploads.size()];
                output.revision = 0;
            }
        }
        published_ = true;
        return {};
    }
}
