#include <lux/engine/render/graph/execute/PhysicalFrameAllocation.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/graph/Range.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/extensions/execution/FramePreparation.hpp>
#include <lux/engine/render/extensions/resource/ResourceRegistry.hpp>
#include <lux/engine/render/graph/execute/RenderGraphInstance.hpp>
#include <lux/engine/render/graph/execute/FrameResources.hpp>
#include <lux/engine/render/device/RasterProgramUpdate.hpp>
#include <lux/engine/render/memory/Cleanup.hpp>
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

    class RenderGraphInstance::ProducerFrame final : public FramePreparation
    {
    public:
        ProducerFrame(RenderGraphInstance& instance, std::uint32_t producer_slot, std::uint32_t frame_slot) noexcept
            : instance_(instance), linked_(instance.preparation_plan_->producers[producer_slot]),
              producer_(instance.producers_[producer_slot]), producer_slot_(producer_slot), frame_slot_(frame_slot)
        {
        }

        const TargetDesc& target() const noexcept override { return instance_.target(); }

        RenderResult<void> setRasterStream(DrawStreamSlot slot, const RasterStreamData& data) noexcept override
        {
            const auto grant = streamRange(slot.index);
            if (!grant) return cxx::unexpected(grant.error());
            auto& stream = producer_.streams[(*grant)->local_stream];
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
            if (!instance_.graph_.programs().rasterStream(slot.index)->runtime)
                return cxx::unexpected(RenderError{kGraphUnknownPass, {slot.index}});
            const auto& selected = instance_.dynamic_raster_programs_[slot.index];
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
            const auto& bound = instance_.draw_resources_[source];
            if (!bound.storage) return cxx::unexpected(RenderError{kStorageIdentity, {pass, source}});
            constexpr auto records = static_cast<std::uint32_t>(EStorageCapability::STORAGE_BUFFER_ARRAY);
            if (!(bound.capabilities & records)) return cxx::unexpected(RenderError{kStorageMismatch, {pass, source}});
            const auto& compiled = *instance_.graph_.programs().rasterStream(pass);
            const auto* dynamic = compiled.runtime ? instance_.dynamic_raster_programs_[pass].get() : nullptr;
            const auto& programs = dynamic ? dynamic->programs : compiled.programs;
            const auto revision = dynamic ? dynamic->revision : 0;
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
            const auto& desc = instance_.session_->nriCore().GetBufferDesc(*input.buffer);
            const bool valid_buffer = (desc.usage & nri::BufferUsageBits::SHADER_RESOURCE) != 0 &&
                desc.structureStride == stride && input.range.offset % stride == 0 && input.range.size == stride &&
                validInterval(input.range.offset, input.range.size, desc.size);
            if (!valid_buffer) return cxx::unexpected(RenderError{kGraphInvalidRange, {pass, source}});
            auto& stream = producer_.streams[instance_.preparation_plan_->draw_resource_ranges[pass].local_stream];
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

        RenderResult<const PreparationPlan::DrawSourceRange*>
        streamRange(std::uint32_t pass) const noexcept
        {
            const auto& ranges = instance_.preparation_plan_->draw_resource_ranges;
            if (pass >= ranges.size() || ranges[pass].producer != producer_slot_)
                return cxx::unexpected(RenderError{kGraphUnknownPass, {pass}});
            return &ranges[pass];
        }

        RenderResult<const PreparationPlan::DrawSourceRange*>
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
            const auto& bound = instance_.draw_resources_[source];
            if (!bound.storage) return cxx::unexpected(RenderError{kStorageIdentity, {pass, source}});
            if (!bound.indexed_geometry) return cxx::unexpected(RenderError{kStorageMismatch, {pass, source}});

            const auto& device = *instance_.session_;
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

            auto& stream = producer_.streams[(*grant)->local_stream];
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

        RenderResult<const PreparationPlan::DrawSourceRange*>
        sampledImageSource(DrawSourceAddress address) const noexcept
        {
            const auto grant = drawSourceRange(address);
            if (!grant) return cxx::unexpected(grant.error());
            const auto [pass, source] = address;
            const auto& bound = instance_.draw_resources_[source];
            constexpr auto capability = static_cast<std::uint32_t>(EStorageCapability::SAMPLED_IMAGE_ARRAY);
            if (!bound.storage || !(bound.capabilities & capability))
                return cxx::unexpected(RenderError{kStorageMismatch, {pass, source}});
            return grant;
        }

        bool validSampledImage(const RasterSampledImageRead& read) const noexcept
        {
            if (!read.image) return false;
            const auto& range = read.range;
            const auto& desc = instance_.session_->nriCore().GetTextureDesc(*read.image);
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
            producer_.streams[(*grant)->local_stream].textures.push_back(read);
            return {};
        }

        RenderResult<const void*>
        readDrawSource(DrawSourceAddress address, cxx::basic_type_info type) const noexcept override
        {
            const auto [pass, source] = address;
            if (auto grant = drawSourceRange(address); !grant) return cxx::unexpected(grant.error());
            const auto& bound = instance_.draw_resources_[source];
            if (bound.storage && bound.implementation != type)
                return cxx::unexpected(RenderError{kStorageMismatch, {pass, source}});
            return bound.storage;
        }

        RenderResult<extension_detail::SelectedStorageSnapshot>
        readStorageValue(extension_detail::SnapshotOperand operand) const noexcept override
        {
            if (!linked_.snapshots.contains(operand.frame))
                return cxx::unexpected(RenderError{kGraphUnknownResource, {kGraphInvalidIndex, operand.frame}});
            const auto& read = instance_.preparation_plan_->snapshot_reads[operand.frame];
            if (read.type != operand.type) return cxx::unexpected(RenderError{kStorageMismatch, {operand.frame}});
            const auto& selection = instance_.storage_snapshots_[instance_.snapshot_sources_[operand.frame]];
            return extension_detail::SelectedStorageSnapshot{selection.value.get(), selection.revision};
        }

        RenderResult<SelectedBytes> readBytes(std::uint32_t slot, std::size_t bytes) const noexcept override
        {
            if (!linked_.inputs.contains(slot))
                return cxx::unexpected(RenderError{kGraphUnknownResource, {kGraphInvalidIndex, slot}});
            const auto input = instance_.preparation_plan_->cpu_reads[slot].index;
            const auto& selection = instance_.cpu_data_[input];
            if (!selection.written)
                return cxx::unexpected(RenderError{kGraphUndefinedRead, {kGraphInvalidIndex, input}});
            if (selection.bytes.size() != bytes)
                return cxx::unexpected(RenderError{kFrameContract, {slot}});
            return SelectedBytes{selection.bytes, instance_.preparation_revision_};
        }

        RenderResult<void> publishBytes(std::uint32_t slot, std::span<const std::byte> bytes) noexcept override
        {
            if (!linked_.outputs.contains(slot))
                return cxx::unexpected(RenderError{kGraphUnknownResource, {kGraphInvalidIndex, slot}});
            return stage(instance_.cpu_data_[slot], bytes);
        }

        RenderResult<void>
        writeInlineBlock(std::uint32_t slot, std::span<const std::byte> bytes, std::uint64_t revision) noexcept override
        {
            if (!linked_.inline_blocks.contains(slot))
                return cxx::unexpected(RenderError{kGraphUnknownPass, {slot}});
            auto& output = producer_.constants[slot - linked_.inline_blocks.first];
            const bool unchanged = revision != 0 && revision == output.source_revision;
            auto written = stage(output.payload, bytes, unchanged);
            if (written) output.source_revision = revision;
            return written;
        }

        RenderResult<void> writeGpuBuffer(
            std::uint32_t slot, std::span<const std::byte> bytes, std::uint64_t revision
        ) noexcept override
        {
            if (!ownsBuffer(slot))
                return cxx::unexpected(RenderError{kGraphUnknownResource, {kGraphInvalidIndex, slot}});
            const auto& contract = instance_.preparation_plan_->buffers[slot];
            auto& selected = instance_.prepared_buffer_inputs_[slot];
            const bool invalid_extent =
                bytes.size() < contract.minimum_bytes || bytes.size() > contract.maximum_bytes || bytes.size() % 4;
            const bool already_written = selected.source != PreparedBufferInput::ESource::MISSING;
            if (!contract.upload || already_written || invalid_extent)
                return cxx::unexpected(RenderError{kFrameContract, {kGraphInvalidIndex, contract.resource}});
            const auto& replica = instance_.frames_[frame_slot_].native.prepared_buffers[slot];
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
            if (!ownsBuffer(slot))
                return cxx::unexpected(RenderError{kGraphUnknownResource, {kGraphInvalidIndex, slot}});
            const auto& contract = instance_.preparation_plan_->buffers[slot];
            auto& selected = instance_.prepared_buffer_inputs_[slot];
            const bool invalid_extent = size < contract.minimum_bytes || size > contract.maximum_bytes || size % 4;
            const bool already_written = selected.source != PreparedBufferInput::ESource::MISSING;
            if (contract.upload || already_written || invalid_extent)
                return cxx::unexpected(RenderError{kFrameContract, {kGraphInvalidIndex, contract.resource}});
            selected.source = PreparedBufferInput::ESource::SIZE_ONLY;
            selected.size = size;
            return {};
        }

        [[nodiscard]] bool ownsBuffer(std::uint32_t slot) const noexcept
        {
            const auto& buffers = instance_.preparation_plan_->buffers;
            return slot < buffers.size() && buffers[slot].producer == producer_slot_;
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

        RenderGraphInstance& instance_;
        const LinkedPreparationProducer& linked_;
        Producer& producer_;
        std::uint32_t producer_slot_;
        std::uint32_t frame_slot_;
    };

    RenderResult<void> RenderGraphInstance::createProducers(
        const PreparationPlan& plan, const ResourceRegistry& registry, PreparationSources inputs
    ) noexcept
    {
        if (inputs.constant_buffers.size() != plan.constant_buffers.size())
            return cxx::unexpected(RenderError{kFrameContract});
        if (plan.producers.empty() && plan.constant_copies.empty() && plan.constant_buffers.empty()) return {};
        if (preparation_plan_) return cxx::unexpected(RenderError{kDeviceArgument});
        const auto& sources = inputs.snapshots;
        if (inputs.effect_values.size() != plan.effect_inputs.size())
            return cxx::unexpected(RenderError{kFrameContract});
        for (std::size_t i = 0; i < plan.effect_inputs.size(); ++i)
        {
            const auto slot = plan.effect_inputs[i].destination.index;
            if (slot >= plan.cpu_data_bytes.size() || inputs.effect_values[i].size() != plan.cpu_data_bytes[slot])
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
            snapshots.emplace_back(source, std::move(value));
        }
        std::vector<extension_detail::AlgorithmWorkspace> workspaces;
        workspaces.reserve(plan.workspace_count);
        std::vector<Producer> producers;
        std::vector<std::uint32_t> program_producers;
        producers.reserve(plan.producers.size());
        for (const auto& linked : plan.producers)
        {
            auto& producer = producers.emplace_back();
            producer.streams.resize(linked.raster_streams.size());
            if (const auto create = linked.call.methods->create_workspace)
            {
                auto workspace = create(linked.call.inputs.data(), target());
                if (!workspace) return cxx::unexpected(workspace.error());
                workspaces.push_back(std::move(*workspace));
            }
            for (auto pass : linked.raster_streams)
            {
                if (graph_.programs().rasterStream(pass)->runtime) producer.runtime_streams.push_back(pass);
            }
            if (!linked.program_snapshots.empty() && producer.runtime_streams.empty())
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {producers.size() - 1}});
            for (auto slot : linked.program_snapshots)
            {
                const auto source = source_slots[plan.snapshot_reads[slot].source];
                auto& snapshot = snapshots[source];
                if (!snapshot.program_input)
                {
                    if (!snapshot.source.ops->program_input)
                        return cxx::unexpected(RenderError{kStorageMismatch, {slot}});
                    snapshot.program_input = &snapshot.source.ops->program_input(snapshot.source.storage);
                }
                auto& selected = producer.program_sources;
                if (std::ranges::find(selected, source) == selected.end()) selected.push_back(source);
            }
            if (!producer.runtime_streams.empty())
            {
                producer.programs_dirty = true;
                program_producers.push_back(static_cast<std::uint32_t>(producers.size() - 1));
            }
            producer.constants.reserve(linked.inline_blocks.count);
            const auto blocks = std::span{plan.inline_blocks}.subspan(
                linked.inline_blocks.first, linked.inline_blocks.count
            );
            for (const auto& block : blocks)
            {
                producer.constants.push_back({{std::vector<std::byte>(block.bytes)}});
            }
        }
        cpu_data_.reserve(plan.cpu_data_bytes.size());
        for (auto bytes : plan.cpu_data_bytes) cpu_data_.push_back({std::vector<std::byte>(bytes)});
        prepared_buffer_inputs_.resize(plan.buffers.size());
        for (auto& frame : frames_)
        {
            frame.native.prepared_buffers.resize(plan.buffers.size());
        }
        workspaces_ = std::move(workspaces);
        producers_ = std::move(producers);
        program_producers_ = std::move(program_producers);
        if (!program_producers_.empty())
        {
            program_input_revision_ = 1;
            dynamic_raster_programs_.resize(graph_.passes().size());
        }
        // Build the Resource-to-kernel links while the execution is private. Publication only attaches rows.
        for (auto index : program_producers_)
        {
            auto& producer = producers_[index];
            for (auto slot : producer.program_sources)
            {
                auto* source = snapshots[slot].program_input;
                auto use = std::ranges::find(program_input_uses_, source, &ProgramInputRevision::Use::source);
                if (use == program_input_uses_.end())
                {
                    program_input_uses_.push_back({source, &program_input_revision_, {&producer.programs_dirty}});
                }
                else use->dirty_programs.push_back(&producer.programs_dirty);
            }
        }
        storage_snapshots_ = std::move(snapshots);
        snapshot_sources_.resize(plan.snapshot_reads.size());
        for (std::size_t i = 0; i < plan.snapshot_reads.size(); ++i)
            snapshot_sources_[i] = source_slots[plan.snapshot_reads[i].source];
        draw_resources_ = std::move(inputs.draw_resources);
        constant_buffers_ = std::move(inputs.constant_buffers);
        effect_values_ = std::move(inputs.effect_values);
        snapshot_registry_ = &registry;
        preparation_plan_ = &plan;
        return {};
    }

    void RenderGraphInstance::releaseDynamicRasterPrograms(std::uint32_t pass, bool replacing) noexcept
    {
        auto& selected = dynamic_raster_programs_[pass];
        if (!selected) return;
        if (replacing && !selected->programs.empty()) releaseRasterBindings(pass, selected->revision);
        const auto get = [&](std::size_t i) noexcept { return selected->programs[i].cache_id; };
        session_->programs().release(selected->programs.size(), get, gpu_.accepted);
        selected.reset();
    }

    bool RenderGraphInstance::hasExclusiveProgramInput() const noexcept
    {
        return std::ranges::any_of(storage_snapshots_, [&](const auto& snapshot) noexcept
        {
            return snapshot.program_input && snapshot_registry_->hasExclusiveCommand(snapshot.source.id);
        });
    }

    void RenderGraphInstance::installPreparation(RenderJobId job, std::uint64_t generation) noexcept
    {
        if (!generation || preparation_generation_) std::terminate();
        preparation_job_ = job;
        preparation_generation_ = generation;
        for (auto& use : program_input_uses_) use.source->attach(use);
    }

    RenderResult<void> RenderGraphInstance::selectProgramUpdate(RasterProgramUpdate& update) noexcept
    {
        update.input_revision = program_input_revision_;
        for (auto index : program_producers_)
        {
            if (!producers_[index].programs_dirty) continue;
            if (auto selected = buildRasterPrograms(index, update); !selected) return selected;
        }
        return {};
    }

    RenderResult<void>
    RenderGraphInstance::buildRasterPrograms(std::uint32_t index, RasterProgramUpdate& update) noexcept
    {
        auto& producer = producers_[index];
        for (auto slot : producer.program_sources)
        {
            auto& snapshot = storage_snapshots_[slot];
            auto captured = snapshot.source.ops->capture(snapshot.source.storage, snapshot.value.get());
            if (!captured) return cxx::unexpected(captured.error());
            if (*captured != snapshot.program_input->value())
                return cxx::unexpected(RenderError{kStorageContract, {slot}});
            snapshot.revision = *captured;
        }
        const auto& linked = preparation_plan_->producers[index];
        RasterProgramBuildData data{*snapshot_registry_, *session_};
        data.streams.reserve(producer.runtime_streams.size());
        for (auto pass : producer.runtime_streams)
        {
            auto& stream = data.streams.emplace_back(pass);
            stream.ready = bool(dynamic_raster_programs_[pass]);
            if (preparation_plan_->draw_resource_ranges.empty()) continue;
            const auto range = preparation_plan_->draw_resource_ranges[pass];
            stream.sources = std::span{draw_resources_}.subspan(range.first, range.count);
        }
        data.snapshots.reserve(linked.program_snapshots.size());
        for (auto slot : linked.program_snapshots)
        {
            const auto& snapshot = storage_snapshots_[snapshot_sources_[slot]];
            data.snapshots.push_back({
                slot, snapshot.source.id, preparation_plan_->snapshot_reads[slot].type,
                {snapshot.value.get(), snapshot.revision}
            });
        }
        RasterProgramBuildContext context{data};
        auto mapping = linked.build_programs(linked.call.inputs.data(), context, producer.draw_mapping.get());
        if (!mapping) return cxx::unexpected(mapping.error());
        update.selections.push_back({index, std::move(*mapping)});
        for (const auto& entry : data.streams)
        {
            if (entry.reuse)
            {
                continue;
            }
            if (!entry.programs) return cxx::unexpected(RenderError{kFrameContract, {entry.pass}});
            const auto& contract = *graph_.programs().rasterStream(entry.pass)->runtime;
            auto& stream = update.streams.emplace_back();
            stream.pass = entry.pass;
            stream.candidate = std::make_unique<DynamicRasterPrograms>();
            const auto& previous = dynamic_raster_programs_[entry.pass];
            if (previous && previous->revision == UINT64_MAX)
                return cxx::unexpected(RenderError{kDeviceCapacity});
            stream.candidate->revision = previous ? previous->revision + 1 : 1;
            stream.candidate->programs.reserve(entry.programs->size());
            stream.programs.reserve(entry.programs->size());
            for (const auto& program : *entry.programs)
            {
                auto link = linkRasterStreamProgram(
                    *session_, {contract.arguments, program.parameters, contract.target,
                                entry.shaders[program.shader.index], program.graphics, program.geometry_locations}
                );
                if (!link) return cxx::unexpected(link.error());
                stream.programs.push_back({std::move(*link), program.source});
            }
        }
        return {};
    }

    void RenderGraphInstance::adoptProgramUpdate(RasterProgramUpdate& update) noexcept
    {
        for (auto& stream : update.streams)
        {
            auto& previous = dynamic_raster_programs_[stream.pass];
            if (previous && previous->programs == stream.candidate->programs) continue;
            releaseDynamicRasterPrograms(stream.pass, true);
            previous = std::move(stream.candidate);
        }
        for (auto& selection : update.selections)
        {
            auto& producer = producers_[selection.producer];
            producer.draw_mapping.swap(selection.draw_mapping);
            producer.programs_dirty = false;
        }
        applied_program_revision_ = update.input_revision;
    }

    RenderResult<void> RenderGraphInstance::prepareProducers(std::uint32_t slot) noexcept
    {
        if (!preparation_plan_) return {};
        if (needsProgramBuild()) return cxx::unexpected(RenderError{kDeviceBusy});
        if (slot >= frames_.size()) return cxx::unexpected(RenderError{kDeviceArgument});
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
            if (snapshot.program_input && *revision != snapshot.revision)
                return cxx::unexpected(RenderError{needsProgramBuild() ? kDeviceBusy : kStorageContract});
            snapshot.revision = *revision;
        }
        ++preparation_revision_;
        for (auto& data : cpu_data_) data.written = false;
        for (auto& buffer : prepared_buffer_inputs_) buffer.source = PreparedBufferInput::ESource::MISSING;

        const auto& linked_producers = preparation_plan_->producers;
        // Select each Effect-owned input once on the owner before invoking any producer.
        for (std::size_t i = 0; i < effect_values_.size(); ++i)
        {
            auto& value = cpu_data_[preparation_plan_->effect_inputs[i].destination.index];
            std::ranges::copy(effect_values_[i], value.bytes.begin());
            value.written = true;
        }
        for (std::size_t i = 0; i < linked_producers.size(); ++i)
        {
            const auto& linked = linked_producers[i];
            auto& producer = producers_[i];
            if (producer.programs_dirty) return cxx::unexpected(RenderError{kDeviceBusy});
            for (auto& block : producer.constants) block.payload.written = false;
            for (auto& stream : producer.streams)
            {
                stream.written = false;
                stream.textures.clear();
                stream.geometry.clear();
                stream.geometry_attributes.clear();
                stream.records.clear();
                stream.record_images.clear();
                stream.selection = preparation_revision_;
            }
            ProducerFrame frame{*this, static_cast<std::uint32_t>(i), slot};
            auto* workspace = linked.workspace_slot == kGraphInvalidIndex ?
                nullptr : workspaces_[linked.workspace_slot].get();
            auto prepared = linked.call.methods->prepare(
                linked.call.inputs.data(), frame, workspace, producer.draw_mapping.get()
            );
            if (!prepared) return prepared;

            for (std::uint32_t j = 0; j < linked.outputs.count; ++j)
            {
                const auto output = linked.outputs.first + j;
                if (!cpu_data_[output].written)
                    return cxx::unexpected(RenderError{kGraphUndefinedRead, {kGraphInvalidIndex, output}});
            }
            for (auto output : linked.buffers)
            {
                if (prepared_buffer_inputs_[output].source == PreparedBufferInput::ESource::MISSING)
                    return cxx::unexpected(RenderError{
                        kGraphUndefinedRead, {kGraphInvalidIndex, preparation_plan_->buffers[output].resource}
                    });
            }
            for (std::size_t b = 0; b < linked.inline_blocks.count; ++b)
            {
                if (!producer.constants[b].payload.written)
                {
                    const auto pass = preparation_plan_->inline_blocks[linked.inline_blocks.first + b].pass;
                    return cxx::unexpected(RenderError{kFrameContract, {pass}});
                }
            }
            for (std::size_t s = 0; s < linked.raster_streams.size(); ++s)
            {
                if (!producer.streams[s].written)
                    return cxx::unexpected(RenderError{kFrameContract, {linked.raster_streams[s]}});
            }
        }

        if (needsProgramBuild()) return cxx::unexpected(RenderError{kDeviceBusy});
        // Commit the complete selection only after every preparation callback has succeeded.
        for (auto& replica : frames_[slot].native.prepared_buffers) replica.staged_bytes = 0;
        for (std::size_t i = 0; i < linked_producers.size(); ++i)
        {
            const auto& linked = linked_producers[i];
            for (std::size_t s = 0; s < linked.raster_streams.size(); ++s)
            {
                std::swap(frames_[slot].stream_inputs[linked.raster_streams[s]], producers_[i].streams[s]);
            }
            const auto blocks = std::span{preparation_plan_->inline_blocks}.subspan(
                linked.inline_blocks.first, linked.inline_blocks.count
            );
            for (std::size_t b = 0; b < blocks.size(); ++b)
            {
                const auto& selected = producers_[i].constants[b];
                auto written = writeInlineConstant(
                    slot, blocks[b].pass, selected.payload.bytes, selected.source_revision
                );
                if (!written) return written;
                if (blocks[b].alternate != kGraphInvalidIndex)
                {
                    auto other = writeInlineConstant(slot, blocks[b].alternate, selected.payload.bytes, selected.source_revision);
                    if (!other) return other;
                }
            }
        }
        for (const auto& copy : preparation_plan_->constant_copies)
        {
            if (auto updated = writeInlineConstant(slot, copy.pass.index, cpu_data_[copy.input.index].bytes, 0); !updated)
                return updated;
        }
        return {};
    }

    RenderResult<bool> RenderGraphInstance::stagePreparedBuffers(
        std::uint32_t slot, ResourceUploader* uploads, std::vector<PreparedBufferCopy>& copies
    ) noexcept
    {
        auto& replicas = frames_[slot].native.prepared_buffers;
        for (std::size_t i = 0; i < replicas.size(); ++i)
        {
            const auto& contract = preparation_plan_->buffers[i];
            if (!contract.used) continue;
            if (contract.upload && !uploads) return cxx::unexpected(RenderError{kDeviceUnsupported});
            auto& replica = replicas[i];
            const auto& input = prepared_buffer_inputs_[i];
            const auto size = input.size;
            if (input.source == PreparedBufferInput::ESource::RESIDENT)
            {
                const bool same_allocation = replica.generation == input.resident_generation;
                if (!same_allocation || !replica.contains(input.revision, size))
                    return cxx::unexpected(RenderError{kFrameContract, {kGraphInvalidIndex, contract.resource}});
                setResource(contract.resource, slot, {replica.buffer, 0, size});
                continue;
            }
            if (size > replica.capacity)
            {
                const auto capacity = std::min(std::bit_ceil(size), contract.maximum_bytes);
                const auto description = graphBufferDescription(*session_, graph_.resources()[contract.resource], capacity);
                if (!description) return cxx::unexpected(description.error());
                nri::Buffer* candidate = nullptr;
                const auto result =
                    session_->nriCore().CreatePlacedBuffer(session_->device(), NriDeviceHeap, *description, candidate);
                if (result != nri::Result::SUCCESS) return cxx::unexpected(nativeError(result));
                setResource(contract.resource, slot, {candidate, 0, size});
                releaseAfterGpu(*session_, replica.buffer);
                replica.buffer = candidate;
                ++replica.generation;
                replica.capacity = capacity;
                replica.accepted_revision = 0;
                replica.accepted_size = 0;
            }
            if (!contract.upload)
            {
                setResource(contract.resource, slot, {replica.buffer, 0, size});
                continue;
            }
            const auto bytes = std::span<const std::byte>{input.bytes};
            if (replica.staged_bytes == size) continue;

            const auto first = uploads->stagedFrameCopyCount();
            const auto offset = replica.staged_bytes;
            auto staged = uploads->stageBufferPrefix({replica.buffer, offset, bytes.subspan(offset)});
            if (!staged) return cxx::unexpected(staged.error());
            if (*staged == 0) return false;
            replica.staged_bytes += *staged;
            copies.push_back({
                contract.producer,
                static_cast<std::uint32_t>(i),
                {first, uploads->stagedFrameCopyCount() - first},
                offset,
                size,
                input.revision
            });
            if (replica.staged_bytes != size) return false;
            setResource(contract.resource, slot, {replica.buffer, 0, size});
        }
        return true;
    }

    void RenderGraphInstance::settlePreparedBuffer(
        std::uint32_t slot, const PreparedBufferCopy& copy, SubmissionPoint accepted, RenderError error
    ) noexcept
    {
        auto& replica = frames_[slot].native.prepared_buffers[copy.buffer];
        if (preparation_plan_->buffers[copy.buffer].producer != copy.producer) std::terminate();
        if (error.type)
        {
            replica.staged_bytes = copy.offset;
            return;
        }
        if (!accepted.serial) std::terminate();
        gpu_.accepted.accept(accepted);
        // An accepted prefix can overwrite bytes of the previous revision.
        const bool complete = replica.staged_bytes == copy.size;
        replica.accepted_revision = complete ? copy.revision : 0;
        replica.accepted_size = complete ? copy.size : 0;
    }
} // namespace lux::render::detail
