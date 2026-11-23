#include <lux/engine/render/runtime/JobCpuPreparation.hpp>
#include <lux/engine/render/frame/FrameError.hpp>
#include <lux/engine/render/runtime/commands/Error.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/device/ImageFormat.hpp>
#include <lux/engine/render/graph/Range.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/graph/execute/RasterFramePreparation.hpp>
#include <lux/engine/render/graph/execute/FrameResources.hpp>
#include <lux/engine/render/graph/execute/RasterStreamPlan.hpp>
#include <cmath>
#include <limits>
#include <tuple>

namespace lux::render::detail
{
    namespace
    {
        template <class Access>
        std::span<const Access> accessGroup(const std::vector<Access>& rows, std::uint32_t first) noexcept
        {
            if (first >= rows.size()) return {};
            return std::span{rows}.subspan(first, rows[first].group_count);
        }

        bool includes(GraphBufferRange range, const RasterBufferSlice& slice) noexcept
        {
            if (slice.offset < range.offset) return false;
            const auto offset = slice.offset - range.offset;
            return offset <= range.size && slice.size <= range.size - offset;
        }

        bool includes(GraphImageRange range, GraphImageRange slice) noexcept
        {
            const bool planes_match = slice.aspects && (slice.aspects & ~range.aspects) == 0;
            const bool starts_inside = slice.first_mip >= range.first_mip && slice.first_layer >= range.first_layer;
            if (!planes_match || !starts_inside) return false;
            return validInterval(slice.first_mip - range.first_mip, slice.mip_count, range.mip_count) &&
                validInterval(slice.first_layer - range.first_layer, slice.layer_count, range.layer_count);
        }

        bool sameAttachment(const nri::AttachmentDesc& a, const nri::AttachmentDesc& b) noexcept
        {
            // Load, store and clear values are fixed by the compiled pass.
            return a.descriptor == b.descriptor && a.resolveDst == b.resolveDst;
        }

        bool sameSection(const RasterDrawList::Section& a, const RasterDrawList::Section& b) noexcept
        {
            const auto fields = [](const RasterDrawList::Section& section) noexcept
            {
                const auto& v = section.viewport;
                return std::tie(section.first_color, section.color_count, section.first_draw, section.draw_count,
                    v.x, v.y, v.width, v.height, v.depthMin, v.depthMax);
            };
            const auto bias = [](const auto& value) noexcept
            {
                return value ? std::tuple{true, value->constant, value->clamp, value->slope}
                             : std::tuple{false, 0.0f, 0.0f, 0.0f};
            };
            const bool same_state = fields(a) == fields(b) && bias(a.depth_bias) == bias(b.depth_bias);
            return same_state && sameAttachment(a.depth, b.depth);
        }

        bool samePacket(const RasterDrawPacket& a, const RasterDrawPacket& b) noexcept
        {
            const auto bindings = [](const RasterDrawPacket& packet) noexcept
            {
                return std::tie(packet.pipeline, packet.bindings.layout, packet.first_descriptor,
                    packet.descriptor_count, packet.first_vertex, packet.vertex_count, packet.parameter_offset,
                    packet.parameter_size, packet.index, packet.index_offset, packet.index_type);
            };
            if (bindings(a) != bindings(b) || a.draw.index() != b.draw.index()) return false;
            if (const auto* linear = std::get_if<nri::DrawDesc>(&a.draw))
            {
                const auto& other = std::get<nri::DrawDesc>(b.draw);
                return std::tie(linear->vertexNum, linear->instanceNum, linear->baseVertex, linear->baseInstance) ==
                    std::tie(other.vertexNum, other.instanceNum, other.baseVertex, other.baseInstance);
            }
            if (const auto* indexed = std::get_if<nri::DrawIndexedDesc>(&a.draw))
            {
                const auto& other = std::get<nri::DrawIndexedDesc>(b.draw);
                return std::tie(indexed->indexNum, indexed->instanceNum, indexed->baseIndex,
                           indexed->baseVertex, indexed->baseInstance) ==
                    std::tie(other.indexNum, other.instanceNum, other.baseIndex, other.baseVertex, other.baseInstance);
            }
            const auto range = [](const RasterDrawPacket::Indirect& draw) noexcept
            {
                return std::tie(
                    draw.arguments, draw.offset, draw.count, draw.stride, draw.count_buffer, draw.count_offset
                );
            };
            return range(std::get<RasterDrawPacket::Indirect>(a.draw)) ==
                range(std::get<RasterDrawPacket::Indirect>(b.draw));
        }

        void coalesceReads(std::vector<RasterDrawList::Read>& reads) noexcept
        {
            std::ranges::sort(reads, [](const auto& a, const auto& b) noexcept
            {
                if (a.physical != b.physical) return std::less<const void*>{}(a.physical, b.physical);
                if (a.usage != b.usage) return a.usage < b.usage;
                if (a.usage == EGraphUse::SAMPLED)
                {
                    const auto key = [](const GraphImageRange& r) noexcept
                    {
                        return std::tuple{r.aspects, r.first_mip, r.mip_count, r.first_layer, r.layer_count};
                    };
                    return key(std::get<GraphImageRange>(a.range)) < key(std::get<GraphImageRange>(b.range));
                }
                const auto& x = std::get<GraphBufferRange>(a.range);
                const auto& y = std::get<GraphBufferRange>(b.range);
                return std::tie(x.offset, x.size) < std::tie(y.offset, y.size);
            });
            std::size_t count = 0;
            for (const auto& read : reads)
            {
                const bool same_access = count && reads[count - 1].physical == read.physical &&
                    reads[count - 1].usage == read.usage;
                if (same_access)
                {
                    auto& previous = reads[count - 1];
                    if (previous.range == read.range)
                    {
                        previous.stages |= read.stages;
                        continue;
                    }
                    if (read.usage == EGraphUse::STORAGE_READ)
                    {
                        auto& merged = std::get<GraphBufferRange>(previous.range);
                        const auto& next = std::get<GraphBufferRange>(read.range);
                        const auto offset = next.offset - merged.offset;
                        if (offset <= merged.size)
                        {
                            merged.size = std::max(merged.size, offset + next.size);
                            previous.stages |= read.stages;
                            continue;
                        }
                    }
                }
                reads[count++] = read;
            }
            reads.resize(count);
        }
    }

    RenderResult<ResourceInput>
    RasterFramePreparation::resolveBuffer(
        std::uint32_t pass, const CompiledRasterStream& stream, const RasterBufferSlice& slice, EGraphUse usage
    ) const noexcept
    {
        if (!slice.size || slice.buffer.selection || slice.buffer.stream != pass)
            return cxx::unexpected(RenderError{kGraphInvalidRange});
        const bool whole = slice.size == RasterBufferSlice::whole;
        if (whole && slice.offset != 0) return cxx::unexpected(RenderError{kGraphInvalidRange});
        const auto ranges = accessGroup(stream.buffers, slice.buffer.index);
        const auto allowed = std::ranges::any_of(ranges, [&](const auto& access) noexcept
        {
            const bool allowed_range = whole ? access.whole_resource :
                access.dynamic_extent || includes(access.range, slice);
            return access.usage == usage && allowed_range;
        });
        if (!allowed) return cxx::unexpected(RenderError{kGraphInvalidRange, {slice.buffer.index}});
        const auto resource = ranges.front().resource;
        auto input = frame_.bindings[resource].input();
        const auto size = whole ? input.size : slice.size;
        const bool in_bounds = size && slice.offset <= input.size && size <= input.size - slice.offset;
        const bool overflow = slice.offset > UINT64_MAX - input.offset;
        if (!input.buffer || !in_bounds || overflow)
            return cxx::unexpected(RenderError{kDeviceArgument, {resource}});
        input.offset += slice.offset;
        input.size = size;
        const auto native_size = frame_.bindings[resource].buffer_description->size;
        if (input.offset > native_size || input.size > native_size - input.offset)
            return cxx::unexpected(RenderError{kDeviceArgument, {resource}});
        return input;
    }

    RenderResult<RasterDescriptorInput>
    RasterFramePreparation::resolveDescriptor(
        std::uint32_t p, const CompiledRasterStream& stream,
        const RasterStreamBinding& entry, const RasterDescriptorField& field
    ) const noexcept
    {
        if (field.kind == EDescriptorKind::SAMPLER)
        {
            const auto* source = std::get_if<RasterSamplerSlot>(&entry.source);
            const bool has_sampler = source && source->stream == p && source->index < stream.samplers.size();
            if (!has_sampler) return cxx::unexpected(RenderError{kFrameContract, {p, entry.argument}});
            const auto& sampler = stream.samplers[source->index];
            if (sampler.comparison != field.comparison_sampler)
                return cxx::unexpected(RenderError{kFrameContract, {p, entry.argument}});
            return RasterDescriptorInput{.sampler = sampler.descriptor};
        }
        if (field.kind == EDescriptorKind::SAMPLED_IMAGE)
        {
            const auto* source = std::get_if<RasterImageSlice>(&entry.source);
            if (!source) return cxx::unexpected(RenderError{kFrameContract, {p, entry.argument}});
            const auto& range = source->range;
            const bool one_aspect = range.aspects == 1 || range.aspects == 2 || range.aspects == 4;
            const bool wrong_depth = field.image_depth && range.aspects != 2;
            const bool wrong_layers = !field.view.image_arrayed && range.layer_count != 1;
            if (!one_aspect || wrong_depth || wrong_layers)
                return cxx::unexpected(RenderError{kGraphInvalidRange, {p, entry.argument}});
            const auto includesSelection = [&](const auto& access) noexcept
            {
                auto envelope = access.range;
                const auto& desc = std::get<GraphImageDesc>(graph_.resources()[access.resource].description);
                if (desc.runtime_layers)
                {
                    const auto* image = frame_.bindings[access.resource].image;
                    if (!image) return false;
                    envelope.layer_count = frame_.bindings[access.resource].image_description->layerNum;
                }
                return includes(envelope, range);
            };
            if (source->image.stream != p) return cxx::unexpected(RenderError{kGraphInvalidRange, {p, entry.argument}});
            const auto ranges = accessGroup(stream.images, source->image.index);
            const auto allowed = std::ranges::find_if(ranges, includesSelection);
            if (allowed == ranges.end())
                return cxx::unexpected(RenderError{kGraphInvalidRange, {p, entry.argument}});

            const bool ambiguous = std::ranges::any_of(ranges, [&](const auto& access) noexcept
            {
                return access.fallback != allowed->fallback && includesSelection(access);
            });
            if (ambiguous) return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {p, entry.argument}});

            auto selected = allowed->resource;
            const auto history = graph_.resources()[selected].history;
            const bool use_fallback = allowed->fallback != kGraphInvalidIndex &&
                history != kGraphInvalidIndex && !frame_.history_valid[history];
            if (use_fallback) selected = allowed->fallback;
            auto* image = frame_.bindings[selected].image;
            if (!image) return cxx::unexpected(RenderError{kDeviceArgument, {p, entry.argument}});
            const auto& description = *frame_.bindings[selected].image_description;
            const auto graph_format = std::get<GraphImageDesc>(graph_.resources()[selected].description).format;
            const auto format = imageFormat(graph_format).native;
            const bool valid_mips = validInterval(range.first_mip, range.mip_count, description.mipNum);
            const bool valid_layers = validInterval(range.first_layer, range.layer_count, description.layerNum);
            const bool image_matches = description.format == format && description.sampleNum == 1 &&
                description.type == nri::TextureType::TEXTURE_2D;
            if (!valid_mips || !valid_layers || !image_matches)
                return cxx::unexpected(RenderError{kGraphInvalidRange, {p, entry.argument}});
            return RasterDescriptorInput{.image = image, .format = format, .range = range};
        }

        const auto* slice = std::get_if<RasterBufferSlice>(&entry.source);
        if (!slice) return cxx::unexpected(RenderError{kFrameContract, {p, entry.argument}});
        auto source = resolveBuffer(p, stream, *slice, stream.argument_uses[entry.argument]);
        if (!source) return cxx::unexpected(source.error());
        const auto minimum = std::max(field.block_size, 4u);
        const auto stride = field.block_size ? field.block_size : 4u;
        const auto alignment = field.kind == EDescriptorKind::UNIFORM_BUFFER
            ? session_.description().memoryAlignment.constantBufferOffset : 4u;
        const bool invalid_alignment = alignment == 0 || source->offset % alignment != 0;
        const bool invalid_size = source->size < minimum ||
            (field.kind == EDescriptorKind::STORAGE_BUFFER && source->size % stride != 0);
        if (invalid_alignment || invalid_size)
            return cxx::unexpected(RenderError{kGraphInvalidRange, {p, entry.argument}});
        return RasterDescriptorInput{source->buffer, source->offset, source->size};
    }

    RenderResult<void>
    RasterFramePreparation::selectDrawBindings(
        std::uint32_t p, const CompiledRasterStream& stream, const RasterStreamDraw& draw,
        const CompiledRasterStream::Program& program
    ) noexcept
    {
        const auto& input = draws_[stream.list];
        auto& inputs = frame_.stream_binding_inputs;
        const auto entries = std::span{input.bindings}.subspan(draw.bindings.first, draw.bindings.count);
        const auto& layout = program.bindings;
        const auto first = inputs.size();
        inputs.resize(first + entries.size());
        auto selected = std::span{inputs}.subspan(first);
        for (const auto& entry : entries)
        {
            if (entry.argument >= program.argument_to_use.size())
                return cxx::unexpected(RenderError{kFrameContract, {p, entry.argument}});
            const auto range = program.argument_to_use[entry.argument];
            if (entry.element >= range.count)
                return cxx::unexpected(RenderError{kFrameContract, {p, entry.argument}});
            const auto index = range.first + entry.element;
            auto& destination = selected[index];
            if (destination.buffer || destination.image || destination.sampler)
                return cxx::unexpected(RenderError{kFrameContract, {p, entry.argument}});
            auto source = resolveDescriptor(p, stream, entry, layout.fields[index]);
            if (!source) return cxx::unexpected(source.error());
            destination = *source;
        }
        return {};
    }

    RenderResult<void>
    RasterFramePreparation::appendDraw(
        std::uint32_t p, const CompiledRasterStream& stream, const RasterStreamDraw& draw,
        const CompiledRasterStream::Program& program
    ) noexcept
    {
        const auto& input = draws_[stream.list];
        auto& candidate = frame_.stream_scratch;
        if (auto bound = selectDrawBindings(p, stream, draw, program); !bound) return bound;
        const auto geometrySlice = [&](const RasterBufferSlice& slice, EGraphUse use) noexcept
            -> RenderResult<const RasterDrawInput::Geometry*>
        {
            const auto row = slice.buffer.index / 2;
            const bool vertex = use == EGraphUse::VERTEX;
            const bool wrong_operand = (slice.buffer.index % 2 == 0) != vertex;
            const bool invalid_slot = slice.buffer.stream != p || slice.buffer.selection != input.selection ||
                row >= input.geometry.size() || wrong_operand;
            if (invalid_slot) return cxx::unexpected(RenderError{kGraphInvalidRange, {p, slice.buffer.index}});
            const auto& geometry = input.geometry[row];
            const auto& selected = vertex ? geometry.binding.vertices : geometry.binding.indices;
            const bool valid_range = slice.size && includes({0, selected.size}, slice) &&
                slice.offset % selected.stride == 0 && slice.size % selected.stride == 0;
            if (!valid_range) return cxx::unexpected(RenderError{kGraphInvalidRange, {p, slice.buffer.index}});
            return &geometry;
        };

        const auto recordGeometryRead =
            [&](const GeometryBufferSlice& source, const RasterBufferSlice& slice, EGraphUse use) noexcept
        {
            const auto stages = use == EGraphUse::VERTEX ? 1u : 0u;
            const GraphBufferRange range{source.offset + slice.offset, slice.size};
            candidate.reads.push_back({source.buffer, range, use, stages});
            return ResourceInput{source.buffer, range.offset, range.size};
        };

        RasterDrawPacket packet;
        packet.pipeline = program.pipeline;
        packet.bindings = program.bindings.native;
        packet.parameter_offset = draw.constants.first;
        packet.parameter_size = draw.constants.count;
        if (draw.vertices.count >= kGraphInvalidIndex - candidate.vertices.size())
            return cxx::unexpected(RenderError{kDeviceCapacity, {p}});
        packet.first_vertex = static_cast<std::uint32_t>(candidate.vertices.size());
        packet.vertex_count = draw.vertices.count;
        const auto vertices = std::span{input.vertices}.subspan(draw.vertices.first, draw.vertices.count);
        if (vertices.size() != program.vertices.size())
            return cxx::unexpected(RenderError{kFrameContract, {p}});
        const auto* linear = std::get_if<DrawVertices>(&draw.draw);
        const auto* indexed = std::get_if<RasterStreamIndexed>(&draw.draw);
        const auto* indirect = std::get_if<RasterStreamIndirect>(&draw.draw);
        const auto origin = indirect && indirect->index ? indirect->index->origin : ERasterIndexOrigin::SLICE;
        const bool buffer_base = origin == ERasterIndexOrigin::BUFFER_BASE;
        if (origin != ERasterIndexOrigin::SLICE && !buffer_base)
            return cxx::unexpected(RenderError{kFrameContract, {p}});
        const RasterDrawInput::GeometryBinding* selected_vertices = nullptr;
        for (const auto& abi : program.vertices)
        {
            const auto vertex = std::ranges::find(vertices, abi.binding, &RasterStreamVertex::binding);
            if (vertex == vertices.end()) return cxx::unexpected(RenderError{kFrameContract, {p}});
            auto source = [&]() noexcept -> RenderResult<ResourceInput>
            {
                if (!vertex->source.buffer.selection)
                {
                    if (buffer_base && abi.rate == EVertexRate::VERTEX)
                        return cxx::unexpected(RenderError{kGraphInvalidRange, {p, abi.binding}});
                    return resolveBuffer(p, stream, vertex->source, EGraphUse::VERTEX);
                }
                auto selected = geometrySlice(vertex->source, EGraphUse::VERTEX);
                if (!selected) return cxx::unexpected(selected.error());
                const auto& geometry = **selected;
                const auto& binding = geometry.binding;
                const auto slots = geometry.layout.attributes;
                const auto attributes = std::span{input.geometry_attributes}.subspan(slots.first, slots.count);
                const bool wrong_stride = abi.rate != EVertexRate::VERTEX ||
                    abi.stride != binding.vertices.stride;
                const bool wrong_attributes = std::ranges::any_of(program.geometry_attributes, [&](const auto& entry)
                {
                    if (entry.binding != abi.binding) return false;
                    const auto match = std::ranges::find(
                        attributes, entry.location, &GeometryAttribute::location
                    );
                    return match == attributes.end() ||
                        match->offset != entry.offset || match->format != entry.format;
                });
                const bool indexed_draw = indexed || (indirect && indirect->index);
                const bool wrong_geometry = selected_vertices && *selected_vertices != binding;
                const bool partial_slice = vertex->source.offset ||
                    vertex->source.size != binding.vertices.size;
                if (wrong_stride || wrong_attributes || (indexed_draw && (wrong_geometry || partial_slice)))
                    return cxx::unexpected(RenderError{kGraphInvalidRange, {p, abi.binding}});
                selected_vertices = &binding;
                return recordGeometryRead(binding.vertices, vertex->source, EGraphUse::VERTEX);
            }();
            if (!source) return cxx::unexpected(source.error());
            std::uint64_t elements = 1;
            if (linear)
            {
                elements = abi.rate == EVertexRate::INSTANCE
                    ? std::uint64_t(linear->first_instance) + linear->instances
                    : std::uint64_t(linear->first) + linear->count;
            }
            if (indexed && abi.rate == EVertexRate::INSTANCE)
                elements = std::uint64_t(indexed->first_instance) + indexed->instances;
            if (!abi.stride || elements > source->size / abi.stride)
                return cxx::unexpected(RenderError{kGraphInvalidRange, {p, abi.binding}});
            const auto offset = buffer_base && abi.rate == EVertexRate::VERTEX ?
                source->offset % abi.stride : source->offset;
            candidate.vertices.push_back({abi.binding, {source->buffer, offset, abi.stride}});
        }
        const auto* index_slice = indexed ? &indexed->indices :
            indirect && indirect->index ? &indirect->index->source : nullptr;
        if (index_slice)
        {
            const auto index_type = indexed ? indexed->type : indirect->index->type;
            const bool is_16_bit = index_type == EIndexType::UINT16;
            const auto stride = is_16_bit ? 2u : 4u;
            const bool invalid_type = !is_16_bit && index_type != EIndexType::UINT32;
            if (invalid_type) return cxx::unexpected(RenderError{kGraphInvalidRange, {p}});
            auto source = [&]() noexcept -> RenderResult<ResourceInput>
            {
                if (!index_slice->buffer.selection)
                {
                    if (selected_vertices || buffer_base)
                        return cxx::unexpected(RenderError{kGraphInvalidRange, {p}});
                    return resolveBuffer(p, stream, *index_slice, EGraphUse::INDEX);
                }
                auto selected = geometrySlice(*index_slice, EGraphUse::INDEX);
                if (!selected) return cxx::unexpected(selected.error());
                const auto& binding = (*selected)->binding;
                const bool wrong_geometry = selected_vertices && *selected_vertices != binding;
                const bool wrong_index = stride != binding.indices.stride ||
                    (indexed && indexed->vertex_offset);
                const bool partial_slice = index_slice->offset || index_slice->size != binding.indices.size;
                const bool invalid_arena = buffer_base && (!selected_vertices || partial_slice ||
                    indirect->maximum_count != 1);
                if (wrong_geometry || wrong_index || invalid_arena)
                    return cxx::unexpected(RenderError{kGraphInvalidRange, {p}});
                return recordGeometryRead(binding.indices, *index_slice, EGraphUse::INDEX);
            }();
            if (!source) return cxx::unexpected(source.error());
            const bool invalid_slice = source->offset % stride || source->size < stride;
            const bool invalid_draw = indexed && (!indexed->count || !indexed->instances ||
                std::uint64_t(indexed->first) + indexed->count > source->size / stride);
            if (invalid_slice || invalid_draw)
                return cxx::unexpected(RenderError{kGraphInvalidRange, {p}});
            packet.index = source->buffer;
            packet.index_offset = buffer_base ? source->offset % stride : source->offset;
            packet.index_type = is_16_bit ? nri::IndexType::UINT16 : nri::IndexType::UINT32;
        }

        if (linear)
        {
            if (!linear->count || !linear->instances) return cxx::unexpected(RenderError{kFrameContract, {p}});
            packet.draw = nri::DrawDesc{
                linear->count, linear->instances, linear->first, linear->first_instance
            };
        }
        else if (indexed)
        {
            packet.draw = nri::DrawIndexedDesc{
                indexed->count, indexed->instances, indexed->first, indexed->vertex_offset,
                indexed->first_instance
            };
        }
        else
        {
            const auto& limits = session_.description();
            const auto maximum = indirect->maximum_count;
            if (!maximum || maximum > limits.other.drawIndirectMaxNum)
                return cxx::unexpected(RenderError{kGraphInvalidRange, {p, maximum}});
            const bool unsupported_multi_draw = maximum > 1 && !session_.enabledFeatures().multi_draw_indirect;
            const bool unsupported_count = indirect->count_buffer && !limits.features.drawIndirectCount;
            if (unsupported_multi_draw || unsupported_count)
                return cxx::unexpected(RenderError{kDeviceUnsupported, {p, maximum}});
            auto arguments = resolveBuffer(p, stream, indirect->arguments, EGraphUse::INDIRECT);
            if (!arguments) return cxx::unexpected(arguments.error());
            const auto bytes = packet.index ? sizeof(nri::DrawIndexedDesc) : sizeof(nri::DrawDesc);
            const auto stride = indirect->stride ? indirect->stride : static_cast<std::uint32_t>(bytes);
            const bool invalid_alignment = arguments->offset % 4 || stride % 4 || stride < bytes;
            const bool too_small = arguments->size < bytes ||
                std::uint64_t(maximum - 1) * stride > arguments->size - bytes;
            if (invalid_alignment || too_small)
                return cxx::unexpected(RenderError{kGraphInvalidRange, {p}});

            RasterDrawPacket::Indirect command{arguments->buffer, arguments->offset, maximum, stride};
            command.mergeable = buffer_base;
            if (indirect->count_buffer)
            {
                auto count = resolveBuffer(p, stream, *indirect->count_buffer, EGraphUse::INDIRECT);
                if (!count) return cxx::unexpected(count.error());
                if (count->offset % 4 || count->size < sizeof(std::uint32_t))
                    return cxx::unexpected(RenderError{kGraphInvalidRange, {p}});
                command.count_buffer = count->buffer;
                command.count_offset = count->offset;
            }
            packet.draw = command;
        }
        candidate.packets.push_back(packet);
        return {};
    }

    void RasterFramePreparation::publishStream(std::uint32_t pass, std::uint32_t list) noexcept
    {
        auto& candidate = frame_.stream_scratch;
        auto& resident = frame_.raster_draws[list];
        const bool same_sections = resident.has_explicit_sections == candidate.has_explicit_sections &&
            (!candidate.has_explicit_sections ||
                (std::ranges::equal(resident.sections, candidate.sections, sameSection) &&
                 std::ranges::equal(resident.colors, candidate.colors, sameAttachment)));
        const bool same_data = resident.descriptors == candidate.descriptors &&
            resident.parameters == candidate.parameters;
        const bool same_vertices = std::ranges::equal(resident.vertices, candidate.vertices,
            [](const auto& a, const auto& b) noexcept
            {
                return a.binding == b.binding && a.buffer.buffer == b.buffer.buffer &&
                    a.buffer.offset == b.buffer.offset && a.buffer.stride == b.buffer.stride;
            });
        if (same_data && same_sections && same_vertices &&
            std::ranges::equal(resident.packets, candidate.packets, samePacket))
        {
            resident.reads.swap(candidate.reads);
            return;
        }
        std::swap(resident, candidate);
        frame_.invalidatePass(graph_, pass);
    }

    RenderResult<void>
    RasterFramePreparation::materializeStream(std::uint32_t p, const CompiledRasterStream& stream) noexcept
    {
        auto& candidate = frame_.stream_scratch;
        auto& inputs = frame_.stream_binding_inputs;
        const auto& input = draws_[stream.list];
        const auto selection = gpu_.selectRasterPrograms(p, stream);
        if (!selection) return cxx::unexpected(selection.error());
        const auto selected_programs = selection->programs;
        candidate.vertices.clear();
        candidate.packets.clear();
        candidate.descriptors.clear();
        candidate.reads.clear();
        candidate.parameters.assign(input.constants.begin(), input.constants.end());
        auto& used_record_programs = frame_.used_record_programs;
        if (!input.records.empty()) used_record_programs.assign(selected_programs.size(), 0);
        inputs.clear();
        inputs.reserve(frame_.stream_descriptor_rows[stream.list].input_count);
        std::uint32_t texture_stages = 0;
        for (const auto& draw : input.draws)
        {
            const auto& program = selected_programs[draw.program.index];
            if (!input.records.empty()) used_record_programs[draw.program.index] = 1;
            texture_stages |= program.shared_texture_stages;
            if (auto prepared = appendDraw(p, stream, draw, program); !prepared) return prepared;
        }
        const auto readImage = [&](RasterSampledImageRead texture, std::uint32_t stages) noexcept
        {
            candidate.reads.push_back({texture.image, texture.range, EGraphUse::SAMPLED, stages});
        };
        for (const auto& record : input.records)
        {
            if (!used_record_programs[record.program]) continue;
            const auto& program = selected_programs[record.program];
            candidate.reads.push_back({
                record.buffer, record.range, EGraphUse::STORAGE_READ, program.record_stages
            });
            for (const auto texture :
                 std::span{input.record_images}.subspan(record.first_image, record.image_count))
            {
                readImage(texture, program.shared_texture_stages);
            }
        }
        if (texture_stages)
        {
            for (const auto texture : input.textures) readImage(texture, texture_stages);
        }
        coalesceReads(candidate.reads);
        if (auto sections = materializeSections(p, input); !sections) return sections;

        for (const auto& write : frame_.stream_descriptor_rows[stream.list].writes)
        {
            const auto& bindings = selected_programs[write.program].bindings;
            const auto selected = std::span{inputs}.subspan(write.first_input, bindings.fields.size());
            auto set = descriptorSet(bindings, write.record, selected);
            if (!set) return cxx::unexpected(set.error());
            auto& packet = candidate.packets[write.draw];
            packet.first_descriptor = static_cast<std::uint32_t>(candidate.descriptors.size());
            packet.descriptor_count = 1;
            candidate.descriptors.push_back(*set);
        }
        if (session_.enabledFeatures().multi_draw_indirect)
            mergeIndexedIndirectDraws(candidate, session_.description().other.drawIndirectMaxNum);
        publishStream(p, stream.list);
        return {};
    }

    RenderResult<void> RasterFramePreparation::materializeStreams() noexcept
    {
        for (const auto pass : graph_.recordingDependencies().producer_passes)
        {
            const auto* stream = graph_.programs().rasterStream(pass);
            if (!stream) continue;
            if (auto prepared = materializeStream(pass, *stream); !prepared) return prepared;
        }
        return {};
    }
} // namespace lux::render::detail
