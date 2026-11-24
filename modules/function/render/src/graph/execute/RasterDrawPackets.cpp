#include <lux/engine/render/graph/execute/RasterDraws.hpp>
#include <lux/engine/render/frame/FrameError.hpp>
#include <lux/engine/render/runtime/commands/Error.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/device/ImageFormat.hpp>
#include <lux/engine/render/graph/Range.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/graph/execute/RasterStreamPlan.hpp>
#include <lux/engine/render/graph/execute/FrameResources.hpp>
#include <cmath>
#include <iterator>
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
                return std::tie(packet.scissor.x, packet.scissor.y, packet.scissor.width, packet.scissor.height,
                    packet.pipeline, packet.bindings, packet.descriptor_set, packet.first_vertex, packet.vertex_count,
                    packet.parameter_offset, packet.parameter_size, packet.index, packet.index_offset, packet.index_type);
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

        bool sameCommands(const RasterDrawList& resident, const RasterDrawList& candidate) noexcept
        {
            if (resident.has_explicit_sections != candidate.has_explicit_sections) return false;
            if (resident.packets.size() != candidate.packets.size()) return false;
            if (candidate.has_explicit_sections &&
                (!std::ranges::equal(resident.sections, candidate.sections, sameSection) ||
                 !std::ranges::equal(resident.colors, candidate.colors, sameAttachment))) return false;

            const auto same_vertex = [](const auto& a, const auto& b) noexcept
            {
                return a.binding == b.binding && a.buffer.buffer == b.buffer.buffer &&
                    a.buffer.offset == b.buffer.offset && a.buffer.stride == b.buffer.stride;
            };
            return resident.parameters == candidate.parameters &&
                std::ranges::equal(resident.vertices, candidate.vertices, same_vertex) &&
                std::ranges::equal(resident.packets, candidate.packets, samePacket);
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

        void collectResourceReads(
            const RasterDrawInput& input, const RasterProgramSelection& programs,
            std::span<const FrameResources::RasterProgramDescriptors> rows, std::vector<RasterDrawList::Read>& reads
        ) noexcept
        {
            const auto readImage = [&](RasterSampledImageRead image, std::uint32_t stages) noexcept
            {
                reads.push_back({image.image, image.range, EGraphUse::SAMPLED, stages});
            };
            for (const auto& record : input.records)
            {
                if (!rows[record.program].used) continue;
                const auto& program = programs[record.program];
                reads.push_back({record.buffer, record.range, EGraphUse::STORAGE_READ, program.record_stages});
                for (const auto image : input.record_images.subspan(record.first_image, record.image_count))
                    readImage(image, program.shared_texture_stages);
            }
            if (!input.textures.empty())
            {
                std::uint32_t stages = 0;
                for (std::size_t i = 0; i < rows.size(); ++i)
                    if (rows[i].used) stages |= programs[i].shared_texture_stages;
                if (stages)
                    for (const auto image : input.textures) readImage(image, stages);
            }
            coalesceReads(reads);
        }
    }

    RenderResult<ResourceInput>
    FrameResources::resolveBuffer(
        const CompiledRasterStream& stream, const RasterBufferSlice& slice, EGraphUse usage
    ) const noexcept
    {
        if (!slice.size || slice.buffer.selection || !stream.acceptsStream(slice.buffer.stream))
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
        auto input = bindings[resource].input();
        const auto size = whole ? input.size : slice.size;
        const bool in_bounds = size && slice.offset <= input.size && size <= input.size - slice.offset;
        const bool overflow = slice.offset > UINT64_MAX - input.offset;
        if (!input.buffer || !in_bounds || overflow)
            return cxx::unexpected(RenderError{kDeviceArgument, {resource}});
        input.offset += slice.offset;
        input.size = size;
        const auto native_size = bindings[resource].buffer_description->size;
        if (input.offset > native_size || input.size > native_size - input.offset)
            return cxx::unexpected(RenderError{kDeviceArgument, {resource}});
        return input;
    }

    RenderResult<RasterDescriptorInput>
    FrameResources::resolveDescriptor(
        const CompiledRasterStream& stream, const RasterStreamBinding& entry, const RasterDescriptorField& field,
        std::uint32_t ordinal
    ) const noexcept
    {
        const auto p = stream.pass;
        if (const auto* requested = std::get_if<RasterTargetImage>(&entry.source))
        {
            const auto key = std::pair{stream.list, ordinal};
            const auto operand = std::ranges::lower_bound(target_bindings, key, {}, &TargetBinding::key);
            if (operand == target_bindings.end() || operand->key() != key || operand->argument != entry.argument)
                return cxx::unexpected(RenderError{kFrameContract, {p, ordinal}});
            const auto& read = target_reads[operand->read];
            if (read.requested != *requested) return cxx::unexpected(RenderError{kFrameContract, {p, ordinal}});
            return RasterDescriptorInput{.target_view = read.view};
        }
        if (field.kind == EDescriptorKind::SAMPLER)
        {
            const auto* source = std::get_if<RasterSamplerSlot>(&entry.source);
            const bool has_sampler = source && stream.acceptsStream(source->stream) && source->index < stream.samplers.size();
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
            const bool wrong_layers = !field.view.acceptsMultipleLayers() && range.layer_count != 1;
            if (!one_aspect || wrong_depth || wrong_layers)
                return cxx::unexpected(RenderError{kGraphInvalidRange, {p, entry.argument}});
            const auto includesSelection = [&](const auto& access) noexcept
            {
                auto envelope = access.range;
                const auto& desc = std::get<GraphImageDesc>(recordingGraph().resources()[access.resource].description);
                if (desc.runtime_layers)
                {
                    const auto* image = bindings[access.resource].image;
                    if (!image) return false;
                    envelope.layer_count = bindings[access.resource].image_description->layerNum;
                }
                return includes(envelope, range);
            };
            if (!stream.acceptsStream(source->image.stream)) return cxx::unexpected(RenderError{kGraphInvalidRange, {p, entry.argument}});
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
            const auto history = recordingGraph().resources()[selected].history;
            const bool use_fallback = allowed->fallback != kGraphInvalidIndex &&
                history != kGraphInvalidIndex && !history_valid[history];
            if (use_fallback) selected = allowed->fallback;
            auto* image = bindings[selected].image;
            if (!image) return cxx::unexpected(RenderError{kDeviceArgument, {p, entry.argument}});
            const auto& description = *bindings[selected].image_description;
            const auto graph_format = std::get<GraphImageDesc>(recordingGraph().resources()[selected].description).format;
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
        auto source = resolveBuffer(stream, *slice, stream.argument_uses[entry.argument]);
        if (!source) return cxx::unexpected(source.error());
        const auto minimum = std::max(field.block_size, 4u);
        const auto stride = field.block_size ? field.block_size : 4u;
        const auto alignment = field.kind == EDescriptorKind::UNIFORM_BUFFER
            ? recordingGraph().rasterDrawLimits().uniform_alignment : 4u;
        const bool invalid_alignment = alignment == 0 || source->offset % alignment != 0;
        const bool invalid_size = source->size < minimum ||
            (field.kind == EDescriptorKind::STORAGE_BUFFER && source->size % stride != 0);
        if (invalid_alignment || invalid_size)
            return cxx::unexpected(RenderError{kGraphInvalidRange, {p, entry.argument}});
        return RasterDescriptorInput{source->buffer, source->offset, source->size};
    }

    RenderResult<void>
    FrameResources::selectDrawBindings(
        const CompiledRasterStream& stream, const RasterDrawInput& input, const RasterStreamDraw& draw,
        const CompiledRasterProgram& program
    ) noexcept
    {
        const auto p = stream.pass;
        auto& inputs = stream_binding_inputs;
        const auto entries = std::span{input.data.bindings}.subspan(draw.bindings.first, draw.bindings.count);
        const auto& layout = program.bindings;
        inputs.assign(entries.size(), RasterDescriptorInput{});
        for (const auto& entry : entries)
        {
            if (entry.argument >= program.argument_to_use.size())
                return cxx::unexpected(RenderError{kFrameContract, {p, entry.argument}});
            const auto range = program.argument_to_use[entry.argument];
            if (entry.element >= range.count)
                return cxx::unexpected(RenderError{kFrameContract, {p, entry.argument}});
            const auto index = range.first + entry.element;
            auto& destination = inputs[index];
            if (destination.buffer || destination.image || destination.sampler || !destination.target_view.isNull())
                return cxx::unexpected(RenderError{kFrameContract, {p, entry.argument}});
            auto source = resolveDescriptor(stream, entry, layout.fields[index], static_cast<std::uint32_t>(&entry - input.data.bindings.data()));
            if (!source) return cxx::unexpected(source.error());
            destination = *source;
        }
        return {};
    }

    RenderResult<void>
    FrameResources::appendDraw(
        const CompiledRasterStream& stream, const RasterDrawInput& input, const RasterStreamDraw& draw,
        const CompiledRasterProgram& program
    ) noexcept
    {
        const auto p = stream.pass;
        auto& candidate = stream_scratch;
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
            const GraphBufferRange range{source.offset + slice.offset, slice.size};
            if constexpr (kCollectReadMetrics)
                candidate.reads.push_back({source.buffer, range, use, use == EGraphUse::VERTEX ? 1u : 0u});
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
        const auto vertices = std::span{input.data.vertices}.subspan(draw.vertices.first, draw.vertices.count);
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
                    return resolveBuffer(stream, vertex->source, EGraphUse::VERTEX);
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
                    return resolveBuffer(stream, *index_slice, EGraphUse::INDEX);
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
            const auto& limits = recordingGraph().rasterDrawLimits();
            const auto maximum = indirect->maximum_count;
            if (!maximum || maximum > limits.indirect_count)
                return cxx::unexpected(RenderError{kGraphInvalidRange, {p, maximum}});
            const bool unsupported_multi_draw = maximum > 1 && !limits.multi_draw;
            const bool unsupported_count = indirect->count_buffer && !limits.indirect_count_buffer;
            if (unsupported_multi_draw || unsupported_count)
                return cxx::unexpected(RenderError{kDeviceUnsupported, {p, maximum}});
            auto arguments = resolveBuffer(stream, indirect->arguments, EGraphUse::INDIRECT);
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
                auto count = resolveBuffer(stream, *indirect->count_buffer, EGraphUse::INDIRECT);
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

    void FrameResources::publishStream(std::uint32_t pass, std::uint32_t list) noexcept
    {
        auto& candidate = stream_scratch;
        auto& resident = raster_draws[list];
        const auto& graph = recordingGraph();
        const auto batch = graph.passes()[pass].submission;
        const auto& commands = command_batches[batch];
        const bool can_reuse = graph.submissions()[batch].reusesPass(pass) && commands.revision == commands.recorded_revision;
        if (can_reuse && sameCommands(resident, candidate))
        {
            if constexpr (kCollectReadMetrics) resident.reads.swap(candidate.reads);
            resident.command_revision = candidate.command_revision;
            return;
        }
        std::swap(resident, candidate);
        invalidatePass(recordingGraph(), pass);
    }

    RenderResult<void>
    FrameResources::materializeStream(JobGpuResources& gpu, const CompiledRasterStream& stream, const RasterDrawInput& input) noexcept
    {
        const auto p = stream.pass;
        auto& resident = raster_draws[stream.list];
        const bool empty_commands = input.data.draws.empty() && input.data.sections.empty();
        if (empty_commands && resident.packets.empty() && !resident.has_explicit_sections) return {};
        auto& candidate = stream_scratch;
        const auto selection = gpu.programSlots(stream);
        if (!selection) return cxx::unexpected(selection.error());
        const auto& selected_programs = *selection;
        auto& descriptor_rows = stream_descriptor_rows[stream.list];
        const auto& graph = recordingGraph();
        const auto& commands = command_batches[stream.batch];
        const bool same_source = input.command_revision && input.command_revision == resident.command_revision;
        const bool reusable = same_source && stream.target_arguments.empty() && !pending_passes.contains(p) &&
            graph.submissions()[stream.batch].reusesPass(p) && commands.revision == commands.recorded_revision;
        if (reusable)
        {
            if constexpr (kCollectReadMetrics)
            {
                const auto is_geometry = [](const RasterDrawList::Read& read) noexcept
                { return read.usage == EGraphUse::VERTEX || read.usage == EGraphUse::INDEX; };
                if (input.records.empty() && input.textures.empty())
                {
                    std::erase_if(resident.reads, [&](const auto& read) { return !is_geometry(read); });
                }
                else
                {
                    candidate.reads.clear();
                    std::ranges::copy_if(resident.reads, std::back_inserter(candidate.reads), is_geometry);
                    collectResourceReads(input, selected_programs, descriptor_rows, candidate.reads);
                    resident.reads.swap(candidate.reads);
                }
            }
            return {};
        }
        // A failed descriptor update may have changed native state before the candidate was complete.
        resident.command_revision = 0;
        candidate.command_revision = input.command_revision;
        candidate.vertices.clear();
        candidate.packets.clear();
        if constexpr (kCollectReadMetrics) candidate.reads.clear();
        candidate.parameters.assign(input.data.constants.begin(), input.data.constants.end());
        for (auto& row : descriptor_rows) row.used = 0;
        for (const auto& draw : input.data.draws)
        {
            const auto& program = selected_programs[draw.program.index];
            if (auto prepared = appendDraw(stream, input, draw, program); !prepared) return prepared;
            auto& row = descriptor_rows[draw.program.index];
            const bool new_bindings = row.select(draw.bindings.first);
            if (program.bindings.fields.empty()) continue;
            const auto record = row.records[row.used - 1];
            if (new_bindings)
            {
                if (auto bound = selectDrawBindings(stream, input, draw, program); !bound) return bound;
                if (auto set = updateDrawSet(gpu.session, record, stream_binding_inputs); !set) return set;
            }
            candidate.packets.back().descriptor_set = native.raster_bindings[record].set;
        }
        if constexpr (kCollectReadMetrics) collectResourceReads(input, selected_programs, descriptor_rows, candidate.reads);
        if (auto sections = materializeSections(gpu.session, p, input); !sections) return sections;

        if (recordingGraph().rasterDrawLimits().multi_draw)
            mergeIndexedIndirectDraws(candidate, recordingGraph().rasterDrawLimits().indirect_count);
        publishStream(p, stream.list);
        return {};
    }

    RenderResult<void> FrameResources::materializeStreams(JobGpuResources& gpu, std::span<const RasterDrawInput> draws) noexcept
    {
        for (const auto& stream : recordingGraph().programs().rasterStreams())
        {
            if (stream.resource_list && !passEnabled(recordingGraph().passes()[stream.pass])) continue;
            if (auto prepared = materializeStream(gpu, stream, draws[stream.list]); !prepared) return prepared;
        }
        return {};
    }
} // namespace lux::render::detail
