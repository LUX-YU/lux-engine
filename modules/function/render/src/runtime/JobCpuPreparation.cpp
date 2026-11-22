#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/graph/Range.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/extensions/execution/FramePreparation.hpp>
#include <lux/engine/render/extensions/resource/ResourceRegistry.hpp>
#include <lux/engine/render/runtime/JobCpuPreparation.hpp>
#include <lux/engine/render/graph/execute/FrameResources.hpp>
#include <lux/engine/render/runtime/RasterProgramInputs.hpp>
#include <lux/engine/render/graph/execute/RasterStreamPlan.hpp>
#include <lux/engine/render/graph/Shader.hpp>
#include <algorithm>
#include <bit>
#include <Extensions/NRIHelper.h>

namespace lux::render::detail
{
    using extension_detail::DrawSourceAddress;

    namespace
    {
        RenderResult<OwnedRasterStream::GeometryLayout> captureGeometryLayout(
            OwnedRasterStream& stream,
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
            return OwnedRasterStream::GeometryLayout{
                {static_cast<std::uint32_t>(first), static_cast<std::uint32_t>(attributes.size())}, alignment
            };
        }

    }

    class JobCpuPreparation::ProducerFrame final : public FramePreparation
    {
    public:
        ProducerFrame(
            JobCpuPreparation& job, std::uint32_t producer_slot, std::uint32_t frame_slot, const TargetDesc& target
        ) noexcept
            : job_(job), linked_(job.graph_.cpuProgram().calls[producer_slot]), producer_slot_(producer_slot),
              frame_slot_(frame_slot), target_(target)
        {
        }

        const TargetDesc& target() const noexcept override { return target_; }

        RenderResult<void> setRasterStream(DrawStreamSlot slot, const RasterStreamData& data) noexcept override
        {
            const auto grant = streamRange(slot.index);
            if (!grant) return cxx::unexpected(grant.error());
            auto& stream = job_.streams_[(*grant)->list];
            if (stream.written) return cxx::unexpected(RenderError{kFrameContract, {slot.index}});
            const bool outside_limits = data.draws.size() >= kGraphInvalidIndex ||
                data.vertices.size() >= kGraphInvalidIndex || data.bindings.size() >= kGraphInvalidIndex ||
                data.constants.size() >= kGraphInvalidIndex || data.sections.size() >= kGraphInvalidIndex;
            if (outside_limits) return cxx::unexpected(RenderError{kDeviceCapacity});
            stream.draws.assign(data.draws.begin(), data.draws.end());
            stream.bindings.assign(data.bindings.begin(), data.bindings.end());
            stream.vertices.assign(data.vertices.begin(), data.vertices.end());
            stream.constants.assign(data.constants.begin(), data.constants.end());
            stream.sections.assign(data.sections.begin(), data.sections.end());
            stream.written = true;
            return {};
        }

        RenderResult<RasterProgramSlot>
        rasterProgram(DrawStreamSlot slot, std::uint32_t selection_index) const noexcept override
        {
            const auto grant = streamRange(slot.index);
            if (!grant) return cxx::unexpected(grant.error());
            const auto& stream = *job_.graph_.programs().rasterStream(slot.index);
            if (!stream.runtime)
                return cxx::unexpected(RenderError{kGraphUnknownPass, {slot.index}});
            const auto* selected = job_.gpu_.dynamicRasterPrograms(stream.list);
            if (!selected || selection_index >= selected->programs.size())
                return cxx::unexpected(RenderError{kFrameContract, {slot.index, selection_index}});
            return RasterProgramSlot{slot.index, selection_index, selected->revision};
        }

    private:
        RenderResult<RasterRecordLayout>
        recordLayout(DrawSourceAddress address, RasterProgramSlot program) const noexcept override
        {
            auto grant = drawSourceRange(address);
            if (!grant) return cxx::unexpected(grant.error());
            const auto [pass, source] = address;
            const auto& bound = job_.draw_resources_[source];
            if (!bound.storage) return cxx::unexpected(RenderError{kStorageIdentity, {pass, source}});
            constexpr auto records = static_cast<std::uint32_t>(EStorageCapability::STORAGE_BUFFER_ARRAY);
            if (!(bound.capabilities & records)) return cxx::unexpected(RenderError{kStorageMismatch, {pass, source}});
            const auto& compiled = *job_.graph_.programs().rasterStream(pass);
            auto selection = job_.gpu_.selectRasterPrograms(pass, compiled);
            if (!selection) return cxx::unexpected(selection.error());
            const auto programs = selection->programs;
            const auto revision = selection->revision;
            const bool valid_program = program.stream == pass && program.index < programs.size() &&
                program.revision == revision && programs[program.index].record_stages;
            if (!valid_program) return cxx::unexpected(RenderError{kFrameContract, {pass, program.index}});
            const auto& selected = programs[program.index];
            return RasterRecordLayout{selected.record_layout, frame_slot_, selected.shared_texture_stages != 0};
        }

        RenderResult<void>
        appendRasterRecord(
            const extension_detail::RasterRecordDestination& destination,
            const RasterRecordInput& input,
            std::optional<DrawSourceAddress> images
        ) noexcept override
        {
            const auto [pass, source] = destination.source;
            const auto stride = destination.layout.block.size;
            if (!input.buffer || !stride) return cxx::unexpected(RenderError{kGraphInvalidRange, {pass, source}});
            const auto& desc = job_.gpu_.session.nriCore().GetBufferDesc(*input.buffer);
            const bool valid_buffer = (desc.usage & nri::BufferUsageBits::SHADER_RESOURCE) != 0 &&
                desc.structureStride == stride && input.range.offset % stride == 0 && input.range.size == stride &&
                validInterval(input.range.offset, input.range.size, desc.size);
            if (!valid_buffer) return cxx::unexpected(RenderError{kGraphInvalidRange, {pass, source}});
            auto& stream = job_.streams_[job_.graph_.cpuProgram().draw_resource_ranges[pass].list];
            const auto first = stream.record_images.size();
            if (input.images.size() > kGraphInvalidIndex - first)
                return cxx::unexpected(RenderError{kDeviceCapacity, {pass}});
            if (!input.images.empty())
            {
                if (!images || images->pass != pass || !destination.layout.reads_images)
                    return cxx::unexpected(RenderError{kStorageMismatch, {pass, source}});
                const auto grant = sampledImageSource(*images);
                if (!grant) return cxx::unexpected(grant.error());
                for (const auto& read : input.images)
                {
                    if (!validSampledImage(read))
                        return cxx::unexpected(RenderError{kGraphInvalidRange, {pass, images->source}});
                }
                stream.record_images.insert(stream.record_images.end(), input.images.begin(), input.images.end());
            }
            stream.records.push_back({
                input.buffer, input.range, destination.program,
                static_cast<std::uint32_t>(first), static_cast<std::uint32_t>(input.images.size())
            });
            return {};
        }

        RenderResult<const CpuProgram::DrawSourceRange*>
        streamRange(std::uint32_t pass) const noexcept
        {
            const auto& ranges = job_.graph_.cpuProgram().draw_resource_ranges;
            if (pass >= ranges.size() || ranges[pass].producer != producer_slot_)
                return cxx::unexpected(RenderError{kGraphUnknownPass, {pass}});
            return &ranges[pass];
        }

        RenderResult<const CpuProgram::DrawSourceRange*>
        drawSourceRange(DrawSourceAddress address) const noexcept
        {
            const auto [pass, source] = address;
            const auto grant = streamRange(pass);
            if (!grant) return cxx::unexpected(grant.error());
            const auto& range = **grant;
            if (source < range.first || source - range.first >= range.count)
                return cxx::unexpected(RenderError{kGraphUnknownResource, {pass, source}});
            return &range;
        }

        RenderResult<RasterIndexedGeometry>
        appendIndexedGeometry(DrawSourceAddress address, const IndexedGeometryInput& input) noexcept override
        {
            const auto [pass, source] = address;
            const auto grant = drawSourceRange(address);
            if (!grant) return cxx::unexpected(grant.error());
            const auto& bound = job_.draw_resources_[source];
            if (!bound.storage) return cxx::unexpected(RenderError{kStorageIdentity, {pass, source}});
            if (!bound.indexed_geometry) return cxx::unexpected(RenderError{kStorageMismatch, {pass, source}});

            const auto& device = job_.gpu_.session;
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

            auto& stream = job_.streams_[(*grant)->list];
            const auto first_index = indices.offset / indices.stride;
            const auto vertex_offset = vertices.offset / vertices.stride;
            const bool exhausted = stream.geometry.size() >= kGraphInvalidIndex / 2;
            if (exhausted || first_index > UINT32_MAX || vertex_offset > INT32_MAX)
                return cxx::unexpected(RenderError{kStorageCapacity, {pass}});

            const auto first = static_cast<std::uint32_t>(stream.geometry.size()) * 2u;
            const auto layout = captureGeometryLayout(stream, input, device, pass);
            if (!layout) return cxx::unexpected(layout.error());
            stream.geometry.push_back({{bound.id, input.item, vertices, indices}, *layout});
            return RasterIndexedGeometry{
                {{first, pass, stream.selection}, 0, vertices.size},
                {{first + 1, pass, stream.selection}, 0, indices.size},
                indices.stride == 2 ? EIndexType::UINT16 : EIndexType::UINT32,
                static_cast<std::uint32_t>(first_index), static_cast<std::int32_t>(vertex_offset)
            };
        }

        RenderResult<const CpuProgram::DrawSourceRange*>
        sampledImageSource(DrawSourceAddress address) const noexcept
        {
            const auto grant = drawSourceRange(address);
            if (!grant) return cxx::unexpected(grant.error());
            const auto [pass, source] = address;
            const auto& bound = job_.draw_resources_[source];
            constexpr auto capability = static_cast<std::uint32_t>(EStorageCapability::SAMPLED_IMAGE_ARRAY);
            if (!bound.storage || !(bound.capabilities & capability))
                return cxx::unexpected(RenderError{kStorageMismatch, {pass, source}});
            return grant;
        }

        bool validSampledImage(const RasterSampledImageRead& read) const noexcept
        {
            if (!read.image) return false;
            const auto& range = read.range;
            const auto& desc = job_.gpu_.session.nriCore().GetTextureDesc(*read.image);
            const bool valid_mips = validInterval(range.first_mip, range.mip_count, desc.mipNum);
            const bool valid_layers = validInterval(range.first_layer, range.layer_count, desc.layerNum);
            const bool sampled = (desc.usage & nri::TextureUsageBits::SHADER_RESOURCE) != 0;
            const bool valid_image = sampled && desc.type == nri::TextureType::TEXTURE_2D && desc.sampleNum == 1 &&
                !nri::nriGetFormatProps(desc.format)->isDepth;
            return valid_image && range.aspects == 1 && valid_mips && valid_layers;
        }

        RenderResult<void>
        appendSampledImage(
            DrawSourceAddress address, const nri::Texture& image, GraphImageRange range
        ) noexcept override
        {
            const auto grant = sampledImageSource(address);
            if (!grant) return cxx::unexpected(grant.error());
            const RasterSampledImageRead read{&image, range};
            if (!validSampledImage(read))
                return cxx::unexpected(RenderError{kGraphInvalidRange, {address.pass, address.source}});
            job_.streams_[(*grant)->list].textures.push_back(read);
            return {};
        }

        RenderResult<const void*>
        readDrawSource(DrawSourceAddress address, cxx::basic_type_info type) const noexcept override
        {
            const auto [pass, source] = address;
            if (auto grant = drawSourceRange(address); !grant) return cxx::unexpected(grant.error());
            const auto& bound = job_.draw_resources_[source];
            if (bound.storage && bound.implementation != type)
                return cxx::unexpected(RenderError{kStorageMismatch, {pass, source}});
            return bound.storage;
        }

        RenderResult<extension_detail::SelectedStorageSnapshot>
        readStorageValue(extension_detail::SnapshotOperand operand) const noexcept override
        {
            if (!linked_.snapshots.contains(operand.frame))
                return cxx::unexpected(RenderError{kGraphUnknownResource, {kGraphInvalidIndex, operand.frame}});
            const auto& read = job_.graph_.cpuProgram().snapshot_reads[operand.frame];
            if (read.type != operand.type) return cxx::unexpected(RenderError{kStorageMismatch, {operand.frame}});
            const auto& selection = job_.storage_snapshots_[job_.snapshot_sources_[read.source]];
            return extension_detail::SelectedStorageSnapshot{selection.value.get(), selection.revision};
        }

        RenderResult<SelectedBytes> readBytes(std::uint32_t slot, std::size_t bytes) const noexcept override
        {
            if (!linked_.inputs.contains(slot))
                return cxx::unexpected(RenderError{kGraphUnknownResource, {kGraphInvalidIndex, slot}});
            const auto input = job_.graph_.cpuProgram().cpu_reads[slot].index;
            const auto& selection = job_.cpu_data_[input];
            if (!selection.written)
                return cxx::unexpected(RenderError{kGraphUndefinedRead, {kGraphInvalidIndex, input}});
            if (selection.bytes.size() != bytes)
                return cxx::unexpected(RenderError{kFrameContract, {slot}});
            return SelectedBytes{selection.bytes, job_.preparation_revision_};
        }

        RenderResult<void> publishBytes(std::uint32_t slot, std::span<const std::byte> bytes) noexcept override
        {
            if (!linked_.outputs.contains(slot))
                return cxx::unexpected(RenderError{kGraphUnknownResource, {kGraphInvalidIndex, slot}});
            return stage(job_.cpu_data_[slot], bytes);
        }

        RenderResult<void>
        writeInlineBlock(std::uint32_t slot, std::span<const std::byte> bytes, std::uint64_t revision) noexcept override
        {
            if (!linked_.inline_blocks.contains(slot))
                return cxx::unexpected(RenderError{kGraphUnknownPass, {slot}});
            auto& output = job_.constants_[slot];
            const bool unchanged = revision != 0 && revision == output.source_revision;
            auto written = stage(output.payload, bytes, unchanged);
            if (written) output.source_revision = revision;
            return written;
        }

        RenderResult<void> writeGpuBuffer(
            std::uint32_t slot, std::span<const std::byte> bytes, std::uint64_t revision
        ) noexcept override
        {
            if (!linked_.buffers.contains(slot))
                return cxx::unexpected(RenderError{kGraphUnknownResource, {kGraphInvalidIndex, slot}});
            const auto& contract = job_.graph_.cpuProgram().buffers[slot];
            auto& selected = job_.prepared_buffer_inputs_[slot];
            const bool invalid_extent =
                bytes.size() < contract.minimum_bytes || bytes.size() > contract.maximum_bytes || bytes.size() % 4;
            const bool already_written = selected.source != PreparedBufferInput::ESource::MISSING;
            if (!contract.upload || already_written || invalid_extent)
                return cxx::unexpected(RenderError{kFrameContract, {kGraphInvalidIndex, contract.resource}});
            const auto& replica = job_.gpu_.frames[frame_slot_].prepared_buffers[slot];
            if (!contract.used) selected.source = PreparedBufferInput::ESource::SIZE_ONLY;
            else if (replica.contains(revision, bytes.size()))
            {
                selected.source = PreparedBufferInput::ESource::RESIDENT;
                selected.resident_generation = replica.generation;
            }
            else
            {
                selected.bytes.assign(bytes.begin(), bytes.end());
                selected.source = PreparedBufferInput::ESource::OWNED_BYTES;
            }
            selected.revision = revision;
            selected.size = bytes.size();
            return {};
        }

        RenderResult<void> sizeGpuBuffer(std::uint32_t slot, std::uint64_t size) noexcept override
        {
            if (!linked_.buffers.contains(slot))
                return cxx::unexpected(RenderError{kGraphUnknownResource, {kGraphInvalidIndex, slot}});
            const auto& contract = job_.graph_.cpuProgram().buffers[slot];
            auto& selected = job_.prepared_buffer_inputs_[slot];
            const bool invalid_extent = size < contract.minimum_bytes || size > contract.maximum_bytes || size % 4;
            const bool already_written = selected.source != PreparedBufferInput::ESource::MISSING;
            if (contract.upload || already_written || invalid_extent)
                return cxx::unexpected(RenderError{kFrameContract, {kGraphInvalidIndex, contract.resource}});
            selected.source = PreparedBufferInput::ESource::SIZE_ONLY;
            selected.size = size;
            return {};
        }

        static RenderResult<void>
        stage(PreparedBytes& output, std::span<const std::byte> bytes, bool unchanged = false) noexcept
        {
            if (output.written || output.bytes.size() != bytes.size())
                return cxx::unexpected(RenderError{kFrameContract});
            if (!unchanged) std::ranges::copy(bytes, output.bytes.begin());
            output.written = true;
            return {};
        }

        JobCpuPreparation& job_;
        const CpuCall& linked_;
        std::uint32_t producer_slot_;
        std::uint32_t frame_slot_;
        const TargetDesc& target_;
    };

    RenderResult<void>
    JobCpuPreparation::bind(const ResourceRegistry& registry, PreparationSources inputs, const TargetDesc& target) noexcept
    {
        const auto& plan = graph_.cpuProgram();
        if (inputs.constant_buffers.size() != plan.constant_buffers.size())
            return cxx::unexpected(RenderError{kFrameContract});
        if (plan.calls.empty() && plan.constant_copies.empty() && plan.constant_buffers.empty()) return {};
        if (snapshot_registry_) return cxx::unexpected(RenderError{kDeviceArgument});
        const auto& sources = inputs.snapshots;
        if (inputs.effect_values.size() != plan.effect_inputs.size())
            return cxx::unexpected(RenderError{kFrameContract});
        for (std::size_t i = 0; i < plan.effect_inputs.size(); ++i)
        {
            const auto slot = plan.effect_inputs[i].destination.index;
            if (slot >= plan.cpu_values.size() || inputs.effect_values[i].size() != plan.cpu_values[slot].size)
                return cxx::unexpected(RenderError{kFrameContract, {slot}});
        }
        if (sources.size() != plan.snapshots.size()) return cxx::unexpected(RenderError{kStorageContract});
        if (inputs.draw_resources.size() != plan.draw_resource_bindings.size())
            return cxx::unexpected(RenderError{kStorageContract});
        std::vector<StorageSnapshot> snapshots;
        snapshots.reserve(sources.size());
        std::vector<std::uint32_t> source_slots;
        source_slots.reserve(sources.size());
        for (std::size_t i = 0; i < sources.size(); ++i)
        {
            const auto& source = sources[i];
            const auto expected = plan.snapshots[i].type;
            const bool invalid_source = !source.storage || !source.ops;
            const bool wrong_type = !invalid_source &&
                (source.ops->type != expected || source.ops->type.name() != expected.name());
            if (invalid_source || wrong_type)
                return cxx::unexpected(RenderError{kStorageMismatch, {i}});
            const auto found = std::ranges::find_if(snapshots, [&](const StorageSnapshot& selected)
            {
                return selected.source.id == source.id;
            });
            source_slots.push_back(static_cast<std::uint32_t>(found - snapshots.begin()));
            if (found != snapshots.end())
            {
                if (found->source.ops != source.ops || found->source.storage != source.storage)
                    return cxx::unexpected(RenderError{kStorageMismatch, {i}});
                continue;
            }
            auto value = source.ops->create();
            if (!value) return cxx::unexpected(RenderError{kStorageContract, {i}});
            snapshots.emplace_back(source, std::move(value), 0, program_inputs_.revisionSource(source.id));
        }
        std::vector<extension_detail::AlgorithmWorkspace> workspaces;
        workspaces.reserve(plan.workspace_count);
        for (const auto& linked : plan.calls)
        {
            if (const auto create = linked.call.methods->create_workspace)
            {
                auto workspace = create(linked.call.inputs.data(), target);
                if (!workspace) return cxx::unexpected(workspace.error());
                workspaces.push_back(std::move(*workspace));
            }
        }
        selected_bytes_.resize(plan.byte_count);
        constants_.reserve(plan.inline_blocks.size());
        for (const auto& block : plan.inline_blocks)
            constants_.push_back({{block.bytes.slice(selected_bytes_)}});
        const auto& recording = graph_.recordingDependencies();
        if (!recording.producer_passes.empty()) streams_.resize(recording.raster_list_count);
        cpu_data_.reserve(plan.cpu_values.size());
        for (const auto& value : plan.cpu_values) cpu_data_.push_back({value.slice(selected_bytes_)});
        prepared_buffer_inputs_.resize(plan.buffers.size());
        for (auto& frame : gpu_.frames) frame.prepared_buffers.resize(plan.buffers.size());
        workspaces_ = std::move(workspaces);
        storage_snapshots_ = std::move(snapshots);
        snapshot_sources_ = std::move(source_slots);
        draw_resources_ = std::move(inputs.draw_resources);
        constant_buffers_ = std::move(inputs.constant_buffers);
        effect_values_ = std::move(inputs.effect_values);
        snapshot_registry_ = &registry;
        return {};
    }

    RenderResult<void>
    JobCpuPreparation::select(FrameResources& destination, std::uint32_t slot, const TargetDesc& target) noexcept
    {
        if (!snapshot_registry_) return {};
        if (program_inputs_.pending()) return cxx::unexpected(RenderError{kDeviceBusy});
        if (slot >= gpu_.frames.size()) return cxx::unexpected(RenderError{kDeviceArgument});
        if (preparation_revision_ == UINT64_MAX) return cxx::unexpected(RenderError{kDeviceCapacity});
        for (const auto& snapshot : storage_snapshots_)
        {
            if (snapshot_registry_->hasExclusiveCommand(snapshot.source.id))
                return cxx::unexpected(RenderError{kDeviceBusy});
        }
        for (auto& snapshot : storage_snapshots_)
        {
            auto revision = snapshot.source.ops->capture(snapshot.source.storage, snapshot.value.get());
            if (!revision) return cxx::unexpected(revision.error());
            if (snapshot.program_input && *revision != snapshot.program_input->value())
                return cxx::unexpected(RenderError{program_inputs_.pending() ? kDeviceBusy : kStorageContract});
            snapshot.revision = *revision;
        }
        ++preparation_revision_;
        for (auto& data : cpu_data_) data.written = false;
        for (auto& buffer : prepared_buffer_inputs_)
        {
            buffer.source = PreparedBufferInput::ESource::MISSING;
            buffer.staged_bytes = 0;
        }

        const auto& linked_producers = graph_.cpuProgram().calls;
        // Select each Effect-owned input once on the owner before invoking any producer.
        for (std::size_t i = 0; i < effect_values_.size(); ++i)
        {
            auto& value = cpu_data_[graph_.cpuProgram().effect_inputs[i].destination.index];
            std::ranges::copy(effect_values_[i], value.bytes.begin());
            value.written = true;
        }
        for (std::size_t i = 0; i < linked_producers.size(); ++i)
        {
            const auto& linked = linked_producers[i];
            for (auto& block : std::span{constants_}.subspan(linked.inline_blocks.first, linked.inline_blocks.count))
                block.payload.written = false;
            const auto streams = graph_.cpuProgram().streamsFor(linked);
            for (const auto& output : streams)
            {
                auto& stream = streams_[output.list];
                stream.written = false;
                stream.textures.clear();
                stream.geometry.clear();
                stream.geometry_attributes.clear();
                stream.records.clear();
                stream.record_images.clear();
                stream.selection = preparation_revision_;
            }
            ProducerFrame frame{*this, static_cast<std::uint32_t>(i), slot, target};
            auto* workspace = linked.workspace_slot == kGraphInvalidIndex ?
                nullptr : workspaces_[linked.workspace_slot].get();
            auto prepared = linked.call.methods->prepare(
                linked.call.inputs.data(), frame, workspace, program_inputs_.mapping(linked.program_slot)
            );
            if (!prepared) return prepared;

            for (std::uint32_t j = 0; j < linked.outputs.count; ++j)
            {
                const auto output = linked.outputs.first + j;
                if (!cpu_data_[output].written)
                    return cxx::unexpected(RenderError{kGraphUndefinedRead, {kGraphInvalidIndex, output}});
            }
            for (std::uint32_t b = 0; b < linked.buffers.count; ++b)
            {
                const auto output = linked.buffers.first + b;
                if (prepared_buffer_inputs_[output].source == PreparedBufferInput::ESource::MISSING)
                    return cxx::unexpected(RenderError{
                        kGraphUndefinedRead, {kGraphInvalidIndex, graph_.cpuProgram().buffers[output].resource}
                    });
            }
            for (std::size_t b = 0; b < linked.inline_blocks.count; ++b)
            {
                if (!constants_[linked.inline_blocks.first + b].payload.written)
                {
                    const auto pass = graph_.cpuProgram().inline_blocks[linked.inline_blocks.first + b].pass;
                    return cxx::unexpected(RenderError{kFrameContract, {pass}});
                }
            }
            for (const auto& stream : streams)
                if (!streams_[stream.list].written) return cxx::unexpected(RenderError{kFrameContract, {stream.pass}});
        }

        if (program_inputs_.pending()) return cxx::unexpected(RenderError{kDeviceBusy});
        // Commit the complete selection only after every preparation callback has succeeded.
        destination.stream_inputs.swap(streams_);
        const auto& blocks = graph_.cpuProgram().inline_blocks;
        for (std::size_t i = 0; i < blocks.size(); ++i)
        {
            const auto& block = blocks[i];
            const auto& selected = constants_[i];
            const auto write = [&](std::uint32_t pass) noexcept
            {
                return destination.writeInlineConstant(graph_, pass, selected.payload.bytes, selected.source_revision);
            };
            auto written = write(block.pass);
            if (!written) return written;
            if (block.alternate != kGraphInvalidIndex)
            {
                auto other = write(block.alternate);
                if (!other) return other;
            }
        }
        for (const auto& copy : graph_.cpuProgram().constant_copies)
        {
            auto updated = destination.writeInlineConstant(graph_, copy.pass.index, cpu_data_[copy.input.index].bytes, 0);
            if (!updated) return updated;
        }
        return {};
    }

} // namespace lux::render::detail
