#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/device/ImageFormat.hpp>
#include <lux/engine/render/graph/Range.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/graph/execute/FrameResources.hpp>
#include <lux/engine/render/graph/execute/RasterStreamPlan.hpp>
#include <cmath>
#include <limits>
#include <tuple>

namespace lux::render::detail
{
    namespace
    {
        bool fits(RasterStreamRange range, std::size_t size) noexcept
        {
            return range.first <= size && range.count <= size - range.first;
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
    }

    RenderResult<void> RenderGraphInstance::prepareRasterSections(std::uint32_t slot) noexcept
    {
        auto& frame = frames_[slot];
        for (auto& image : frame.layered_images) image.requested_layers = 0;

        for (std::uint32_t p = 0; p < frame.stream_inputs.size(); ++p)
        {
            if (!programs_->rasterStream(p)) continue;
            const auto& stream = frame.stream_inputs[p];
            if (!stream.written) return cxx::unexpected(RenderError{kFrameContract, {p}});
            const auto layer_count = static_cast<std::uint32_t>(stream.sections.size());
            const auto limit = session_->description().dimensions.textureLayerMaxNum;
            if (layer_count > limit) return cxx::unexpected(RenderError{kGraphInvalidRange, {p, layer_count}});

            const auto matchesLayers = [&](const ResourceAccess& access) noexcept
            {
                const auto resource = access.version.resource;
                const auto& desc = std::get<GraphImageDesc>(plan_->resources()[resource.index].description);
                const auto& range = std::get<GraphImageRange>(access.range);
                if (!desc.runtime_layers) return !layer_count || range.layer_count == layer_count;
                if (!layer_count) return false;
                const auto image = std::ranges::find(
                    frame.layered_images, resource, &FrameResources::LayeredImage::resource
                );
                if (image == frame.layered_images.end()) return false;
                if (image->requested_layers && image->requested_layers != layer_count) return false;
                image->requested_layers = layer_count;
                return true;
            };
            const auto validAttachment = [&](const RasterAttachment& attachment) noexcept
            {
                return matchesLayers(attachment.access) && (!attachment.resolve || matchesLayers(*attachment.resolve));
            };
            const auto& scope = *frame.passes[p].compiled_pass->rasterScope();
            if (scope.section_depth_bias && stream.sections.empty())
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {p}});
            const bool compatible = std::ranges::all_of(scope.colors, validAttachment) &&
                (!scope.depth || validAttachment(*scope.depth));
            if (!compatible) return cxx::unexpected(RenderError{kGraphInvalidRange, {p, layer_count}});
            if (!layer_count) continue;

            auto& seen = frame.stream_layers_seen;
            seen.assign(layer_count, 0);
            std::uint32_t next_draw = 0;
            for (const auto& section : stream.sections)
            {
                const bool covers_next_draws =
                    section.draws.first == next_draw && fits(section.draws, stream.draws.size());
                const bool new_layer = section.layer < layer_count && !seen[section.layer];
                if (!covers_next_draws || !new_layer)
                    return cxx::unexpected(RenderError{kGraphInvalidRange, {p, section.layer}});
                if (section.viewport)
                {
                    if (auto valid = section.viewport->validate(); !valid) return valid;
                }
                const bool invalid_bias = section.depth_bias &&
                    (!std::isfinite(section.depth_bias->constant) || !std::isfinite(section.depth_bias->slope));
                const bool wrong_bias_contract = section.depth_bias.has_value() != scope.section_depth_bias;
                if (invalid_bias || wrong_bias_contract)
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {p, section.layer}});
                next_draw += section.draws.count;
                seen[section.layer] = 1;
            }
            if (next_draw != stream.draws.size())
                return cxx::unexpected(RenderError{kGraphInvalidRange, {p, next_draw}});
        }

        // Every stream has agreed on the shape before any native attachment is replaced.
        for (const auto& image : frame.layered_images)
        {
            if (!image.requested_layers) continue;
            if (auto selection = prepareImageLayers(image.resource, slot, image.requested_layers); !selection)
                return cxx::unexpected(selection.error());
        }
        return {};
    }

    RenderResult<void> RenderGraphInstance::materializeRasterSections(
        std::uint32_t slot, std::uint32_t p, const OwnedRasterStream& stream
    ) noexcept
    {
        auto& frame = frames_[slot];
        auto& candidate = frame.stream_scratch;
        candidate.has_explicit_sections = !stream.sections.empty();
        candidate.sections.clear();
        candidate.colors.clear();
        if (!candidate.has_explicit_sections) return {};

        const auto& pass = frame.passes[p];
        const auto& scope = *pass.compiled_pass->rasterScope();
        const auto& first = scope.colors.empty() ? *scope.depth : scope.colors.front();
        const auto& first_range = std::get<GraphImageRange>(first.access.range);
        const auto& core = session_->nriCore();
        const auto* image = frame.bindings[first.access.version.resource.index].image;
        if (!image) return cxx::unexpected(RenderError{kDeviceArgument, {p}});
        const auto& info = core.GetTextureDesc(*image);
        const auto width = std::max(1u, std::uint32_t(info.width) >> first_range.first_mip);
        const auto height = std::max(1u, std::uint32_t(info.height) >> first_range.first_mip);
        const auto& default_viewport = scope.viewport ? *scope.viewport : frame.viewport;

        const auto sliceView = [&](const ResourceAccess& access, bool depth, std::uint32_t layer,
                                   nri::Descriptor*& view) noexcept -> RenderResult<void>
        {
            if (!view) return cxx::unexpected(RenderError{kDeviceArgument, {p}});
            const auto resource = access.version.resource;
            const auto& desc = std::get<GraphImageDesc>(plan_->resources()[resource.index].description);
            if (desc.runtime_layers)
            {
                const auto image = std::ranges::find(
                    frame.layered_images, resource, &FrameResources::LayeredImage::resource
                );
                if (image == frame.layered_images.end() || layer >= image->views.size())
                    return cxx::unexpected(RenderError{kFrameContract, {p, layer}});
                view = image->views[layer];
                return {};
            }
            auto range = std::get<GraphImageRange>(access.range);
            if (range.layer_count == 1) return {};
            range.first_layer += layer;
            range.layer_count = 1;
            auto* texture = frame.bindings[access.version.resource.index].image;
            if (!texture) return cxx::unexpected(RenderError{kDeviceArgument, {p}});
            auto& views = frame.raster_attachment_views;
            const auto found = std::ranges::find_if(views, [&](const auto& entry) noexcept
            {
                return entry.image == texture && entry.range == range && entry.depth == depth;
            });
            if (found != views.end())
            {
                view = found->view;
                return {};
            }
            const auto& native = core.GetTextureDesc(*texture);
            const auto kind = depth ? nri::TextureView::DEPTH_STENCIL_ATTACHMENT : nri::TextureView::COLOR_ATTACHMENT;
            const nri::TextureViewDesc description{
                texture, kind, native.format, static_cast<nri::Dim_t>(range.first_mip), 1,
                static_cast<nri::Dim_t>(range.first_layer), 1
            };
            nri::Descriptor* selected = nullptr;
            const auto status = core.CreateTextureView(description, selected);
            if (status != nri::Result::SUCCESS) return cxx::unexpected(nativeError(status));
            views.push_back({texture, range, depth, selected});
            view = selected;
            return {};
        };
        const auto sliceAttachment = [&](const RasterAttachment& attachment, bool depth, std::uint32_t layer,
                                         nri::AttachmentDesc& native) noexcept -> RenderResult<void>
        {
            auto sliced = sliceView(attachment.access, depth, layer, native.descriptor);
            if (!sliced) return sliced;
            if (attachment.resolve) return sliceView(*attachment.resolve, depth, layer, native.resolveDst);
            return {};
        };

        for (const auto& selected : stream.sections)
        {
            RasterDrawList::Section section;
            section.first_color = static_cast<std::uint32_t>(candidate.colors.size());
            section.color_count = static_cast<std::uint32_t>(scope.colors.size());
            section.first_draw = selected.draws.first;
            section.draw_count = selected.draws.count;
            const auto& viewport = selected.viewport ? *selected.viewport : default_viewport;
            if (auto valid = viewport.validate(); !valid) return valid;
            section.viewport = {
                viewport.x * width, viewport.y * height, viewport.width * width, viewport.height * height,
                viewport.min_depth, viewport.max_depth, true
            };
            const auto& v = section.viewport;
            const auto max_origin = std::numeric_limits<std::int16_t>::max();
            if (v.x > max_origin || v.y > max_origin)
                return cxx::unexpected(RenderError{kGraphInvalidRange, {p, selected.layer}});
            const nri::Rect scissor{
                static_cast<std::int16_t>(v.x), static_cast<std::int16_t>(v.y),
                static_cast<nri::Dim_t>(v.width), static_cast<nri::Dim_t>(v.height)
            };
            for (auto& packet : std::span{candidate.packets}.subspan(section.first_draw, section.draw_count))
                packet.scissor = scissor;
            if (selected.depth_bias)
            {
                section.depth_bias = nri::DepthBiasDesc{selected.depth_bias->constant, 0, selected.depth_bias->slope};
            }
            for (std::size_t c = 0; c < scope.colors.size(); ++c)
            {
                auto native = pass.colors[c];
                auto sliced = sliceAttachment(scope.colors[c], false, selected.layer, native);
                if (!sliced) return sliced;
                candidate.colors.push_back(native);
            }
            if (scope.depth)
            {
                section.depth = pass.depth;
                auto sliced = sliceAttachment(*scope.depth, true, selected.layer, section.depth);
                if (!sliced) return sliced;
            }
            candidate.sections.push_back(section);
        }
        return {};
    }

    RenderResult<void> RenderGraphInstance::materializeRasterStreams(std::uint32_t slot) noexcept
    {
        auto& frame = frames_[slot];
        if (frame.stream_inputs.empty()) return {};
        const auto buffer = [&](const CompiledRasterStream& stream, const RasterBufferSlice& slice, EGraphUse usage)
            noexcept -> RenderResult<ResourceInput>
        {
            if (!slice.size || slice.buffer.selection || slice.buffer.stream != kGraphInvalidIndex)
                return cxx::unexpected(RenderError{kGraphInvalidRange});
            const bool whole = slice.size == RasterBufferSlice::whole;
            if (whole && slice.offset != 0) return cxx::unexpected(RenderError{kGraphInvalidRange});
            const auto allowed = std::ranges::any_of(stream.buffers, [&](const auto& access) noexcept
            {
                const bool allowed_range = whole ? access.whole_resource :
                    access.dynamic_extent || includes(access.range, slice);
                return access.resource == slice.buffer.index && access.usage == usage && allowed_range;
            });
            if (!allowed) return cxx::unexpected(RenderError{kGraphInvalidRange, {slice.buffer.index}});
            auto input = resourceInput({slice.buffer.index}, slot);
            const auto size = whole ? input.size : slice.size;
            const bool in_bounds = size && slice.offset <= input.size && size <= input.size - slice.offset;
            const bool overflow = slice.offset > UINT64_MAX - input.offset;
            if (!input.buffer || !in_bounds || overflow)
                return cxx::unexpected(RenderError{kDeviceArgument, {slice.buffer.index}});
            input.offset += slice.offset;
            input.size = size;
            const auto native_size = session_->nriCore().GetBufferDesc(*input.buffer).size;
            if (input.offset > native_size || input.size > native_size - input.offset)
                return cxx::unexpected(RenderError{kDeviceArgument, {slice.buffer.index}});
            return input;
        };

        auto& candidate = frame.stream_scratch;
        auto& inputs = frame.stream_binding_inputs;
        for (std::uint32_t p = 0; p < frame.stream_inputs.size(); ++p)
        {
            const auto* stream = programs_->rasterStream(p);
            if (!stream) continue;
            auto& input = frame.stream_inputs[p];
            const auto* dynamic = stream->runtime ? dynamic_raster_programs_[p].get() : nullptr;
            if (stream->runtime && !dynamic) return cxx::unexpected(RenderError{kFrameContract, {p}});
            const auto& selected_programs = dynamic ? dynamic->programs : stream->programs;
            const auto program_revision = dynamic ? dynamic->revision : 0;

            const auto& pass = plan_->passes()[p];
            const auto& command = std::get<RasterListCommands>(plan_->rasterCommands(pass)->draw);

            candidate.vertices.clear();
            candidate.packets.clear();
            candidate.descriptors.clear();
            candidate.reads.clear();
            candidate.parameters.assign(input.constants.begin(), input.constants.end());
            auto& used_material_programs = input.used_material_programs;
            if (!input.materials.empty()) used_material_programs.assign(selected_programs.size(), 0);

            const auto meshBuffer = [&](const RasterBufferSlice& slice, EGraphUse use) noexcept
                -> RenderResult<const OwnedRasterStream::MeshBuffer*>
            {
                const bool invalid_slot = slice.buffer.stream != p || slice.buffer.selection != input.selection ||
                    slice.buffer.index >= input.mesh_buffers.size();
                if (invalid_slot) return cxx::unexpected(RenderError{kGraphInvalidRange, {p, slice.buffer.index}});
                const auto& selected = input.mesh_buffers[slice.buffer.index];
                const auto stride = use == EGraphUse::VERTEX ? selected.asset->vertex_stride :
                    selected.asset->index_stride;
                const bool invalid_range = !slice.size || !includes({0, selected.size}, slice) ||
                    slice.offset % stride || slice.size % stride;
                if (selected.use != use || invalid_range)
                    return cxx::unexpected(RenderError{kGraphInvalidRange, {p, slice.buffer.index}});
                return &selected;
            };

            const auto recordMeshRead =
                [&](const OwnedRasterStream::MeshBuffer& source, const RasterBufferSlice& slice) noexcept
            {
                const auto stages = source.use == EGraphUse::VERTEX ? 1u : 0u;
                const GraphBufferRange range{source.offset + slice.offset, slice.size};
                candidate.reads.push_back({source.buffer, range, source.use, stages});
                return ResourceInput{source.buffer, range.offset, range.size};
            };

            const auto descriptorInput = [&](const RasterStreamBinding& entry, const DescriptorUse& use,
                                              const ShaderBinding& shader) noexcept
                -> RenderResult<RasterDescriptorInput>
            {
                if (use.kind == EDescriptorKind::SAMPLER)
                {
                    const auto* source = std::get_if<RasterSamplerSlot>(&entry.source);
                    const bool has_sampler = source && source->stream == p && source->index < stream->samplers.size();
                    if (!has_sampler) return cxx::unexpected(RenderError{kFrameContract, {p, entry.argument}});
                    const auto& sampler = stream->samplers[source->index];
                    if (sampler.comparison != use.comparison_sampler)
                        return cxx::unexpected(RenderError{kFrameContract, {p, entry.argument}});
                    return RasterDescriptorInput{.sampler = sampler.descriptor};
                }
                if (use.kind == EDescriptorKind::SAMPLED_IMAGE)
                {
                    const auto* source = std::get_if<RasterImageSlice>(&entry.source);
                    if (!source) return cxx::unexpected(RenderError{kFrameContract, {p, entry.argument}});
                    const auto& range = source->range;
                    const bool one_aspect = range.aspects == 1 || range.aspects == 2 || range.aspects == 4;
                    const bool wrong_depth = shader.image_depth && range.aspects != 2;
                    const bool wrong_layers = !shader.image_arrayed && range.layer_count != 1;
                    if (!one_aspect || wrong_depth || wrong_layers)
                        return cxx::unexpected(RenderError{kGraphInvalidRange, {p, entry.argument}});
                    const auto includesSelection = [&](const auto& access) noexcept
                    {
                        if (access.resource != source->image.index) return false;
                        auto envelope = access.range;
                        const auto& desc = std::get<GraphImageDesc>(plan_->resources()[access.resource].description);
                        if (desc.runtime_layers)
                        {
                            const auto* image = frame.bindings[access.resource].image;
                            if (!image) return false;
                            envelope.layer_count = session_->nriCore().GetTextureDesc(*image).layerNum;
                        }
                        return includes(envelope, range);
                    };
                    const auto allowed = std::ranges::find_if(stream->images, includesSelection);
                    if (allowed == stream->images.end())
                        return cxx::unexpected(RenderError{kGraphInvalidRange, {p, entry.argument}});

                    const bool ambiguous = std::ranges::any_of(stream->images, [&](const auto& access) noexcept
                    {
                        return access.fallback != allowed->fallback && includesSelection(access);
                    });
                    if (ambiguous) return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {p, entry.argument}});

                    auto selected = allowed->resource;
                    const auto history = plan_->resources()[selected].history;
                    const bool use_fallback = allowed->fallback != kGraphInvalidIndex &&
                        history != kGraphInvalidIndex && !frame.history_valid[history];
                    if (use_fallback) selected = allowed->fallback;
                    auto* image = frame.bindings[selected].image;
                    if (!image) return cxx::unexpected(RenderError{kDeviceArgument, {p, entry.argument}});
                    const auto& description = session_->nriCore().GetTextureDesc(*image);
                    const auto graph_format = std::get<GraphImageDesc>(plan_->resources()[selected].description).format;
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
                auto source = buffer(*stream, *slice, stream->argument_uses[entry.argument]);
                if (!source) return cxx::unexpected(source.error());
                const auto minimum = std::max(shader.block.size, 4u);
                const auto stride = shader.block.size ? shader.block.size : 4u;
                const auto alignment = shader.kind == EDescriptorKind::UNIFORM_BUFFER
                    ? session_->description().memoryAlignment.constantBufferOffset : 4u;
                const bool invalid_alignment = alignment == 0 || source->offset % alignment != 0;
                const bool invalid_size = source->size < minimum ||
                    (shader.kind == EDescriptorKind::STORAGE_BUFFER && source->size % stride != 0);
                if (invalid_alignment || invalid_size)
                    return cxx::unexpected(RenderError{kGraphInvalidRange, {p, entry.argument}});
                return RasterDescriptorInput{source->buffer, source->offset, source->size};
            };

            const auto setInputs = [&](const RasterStreamDraw& draw, const PassBindings& bindings) noexcept
                -> RenderResult<void>
            {
                const auto entries = std::span{input.bindings}.subspan(draw.bindings.first, draw.bindings.count);
                if (entries.size() != bindings.uses.size())
                    return cxx::unexpected(RenderError{kFrameContract, {p}});
                inputs.assign(bindings.uses.size(), {});
                for (const auto& entry : entries)
                {
                    const auto expected = std::ranges::find_if(bindings.uses, [&](const auto& use) noexcept
                    {
                        const auto argument = use.kind == EDescriptorKind::SAMPLER ? use.sampler : use.resource;
                        return argument == entry.argument && use.element == entry.element;
                    });
                    if (expected == bindings.uses.end())
                        return cxx::unexpected(RenderError{kFrameContract, {p, entry.argument}});
                    auto& input = inputs[expected - bindings.uses.begin()];
                    if (input.buffer || input.image || input.sampler)
                        return cxx::unexpected(RenderError{kFrameContract, {p, entry.argument}});
                    auto source = descriptorInput(entry, *expected, bindings.bindings[expected->field]);
                    if (!source) return cxx::unexpected(source.error());
                    input = *source;
                }
                return {};
            };

            std::uint32_t texture_stages = 0;
            for (const auto& draw : input.draws)
            {
                const bool invalid_program = draw.program.stream != p || draw.program.index >= selected_programs.size() ||
                    draw.program.revision != program_revision;
                const bool invalid_ranges = !fits(draw.constants, input.constants.size()) ||
                    !fits(draw.vertices, input.vertices.size()) || !fits(draw.bindings, input.bindings.size());
                if (invalid_program || invalid_ranges)
                    return cxx::unexpected(RenderError{kFrameContract, {p}});
                const auto& program = selected_programs[draw.program.index];
                if (!input.materials.empty()) used_material_programs[draw.program.index] = 1;
                texture_stages |= program.shared_texture_stages;
                if (draw.constants.count != program.bindings.parameter_bytes || draw.constants.first % 4 != 0)
                    return cxx::unexpected(RenderError{kFrameContract, {p}});
                if (auto bound = setInputs(draw, program.bindings); !bound) return bound;

                RasterDrawPacket packet;
                packet.pipeline = program.pipeline;
                packet.bindings = program.bindings.recordBindings();
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
                const bool mesh_arena = origin == ERasterIndexOrigin::MESH_ARENA;
                if (origin != ERasterIndexOrigin::SLICE && !mesh_arena)
                    return cxx::unexpected(RenderError{kFrameContract, {p}});
                const MeshDescription* selected_vertices = nullptr;
                for (const auto& abi : program.vertices)
                {
                    const auto vertex = std::ranges::find(vertices, abi.binding, &RasterStreamVertex::binding);
                    if (vertex == vertices.end()) return cxx::unexpected(RenderError{kFrameContract, {p}});
                    auto source = [&]() noexcept -> RenderResult<ResourceInput>
                    {
                        if (!vertex->source.buffer.selection)
                        {
                            if (mesh_arena && abi.rate == EVertexRate::VERTEX)
                                return cxx::unexpected(RenderError{kGraphInvalidRange, {p, abi.binding}});
                            return buffer(*stream, vertex->source, EGraphUse::VERTEX);
                        }
                        auto selected = meshBuffer(vertex->source, EGraphUse::VERTEX);
                        if (!selected) return cxx::unexpected(selected.error());
                        const auto& asset = *(*selected)->asset;
                        const bool wrong_stride = abi.rate != EVertexRate::VERTEX || abi.stride != asset.vertex_stride;
                        const bool wrong_attributes = std::ranges::any_of(program.mesh_attributes, [&](const auto& entry)
                        {
                            if (entry.binding != abi.binding) return false;
                            const auto match = std::ranges::find(
                                asset.attributes, entry.location, &MeshAttribute::location
                            );
                            return match == asset.attributes.end() ||
                                match->offset != entry.offset || match->format != entry.format;
                        });
                        const bool indexed_draw = indexed || (indirect && indirect->index);
                        const bool wrong_mesh = selected_vertices && selected_vertices != &asset;
                        const bool partial_slice = vertex->source.offset || vertex->source.size != (*selected)->size;
                        if (wrong_stride || wrong_attributes || (indexed_draw && (wrong_mesh || partial_slice)))
                            return cxx::unexpected(RenderError{kGraphInvalidRange, {p, abi.binding}});
                        selected_vertices = &asset;
                        return recordMeshRead(**selected, vertex->source);
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
                    const auto offset = mesh_arena && abi.rate == EVertexRate::VERTEX ?
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
                            if (selected_vertices || mesh_arena)
                                return cxx::unexpected(RenderError{kGraphInvalidRange, {p}});
                            return buffer(*stream, *index_slice, EGraphUse::INDEX);
                        }
                        auto selected = meshBuffer(*index_slice, EGraphUse::INDEX);
                        if (!selected) return cxx::unexpected(selected.error());
                        const auto* asset = (*selected)->asset;
                        const bool wrong_mesh = selected_vertices && selected_vertices != asset;
                        const bool wrong_index = stride != asset->index_stride || (indexed && indexed->vertex_offset);
                        const bool partial_slice = index_slice->offset || index_slice->size != (*selected)->size;
                        const bool invalid_arena = mesh_arena && (!selected_vertices || partial_slice ||
                            indirect->maximum_count != 1);
                        if (wrong_mesh || wrong_index || invalid_arena)
                            return cxx::unexpected(RenderError{kGraphInvalidRange, {p}});
                        return recordMeshRead(**selected, *index_slice);
                    }();
                    if (!source) return cxx::unexpected(source.error());
                    const bool invalid_slice = source->offset % stride || source->size < stride;
                    const bool invalid_draw = indexed && (!indexed->count || !indexed->instances ||
                        std::uint64_t(indexed->first) + indexed->count > source->size / stride);
                    if (invalid_slice || invalid_draw)
                        return cxx::unexpected(RenderError{kGraphInvalidRange, {p}});
                    packet.index = source->buffer;
                    packet.index_offset = mesh_arena ? source->offset % stride : source->offset;
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
                    const auto& limits = session_->description();
                    const auto maximum = indirect->maximum_count;
                    if (!maximum || maximum > limits.other.drawIndirectMaxNum)
                        return cxx::unexpected(RenderError{kGraphInvalidRange, {p, maximum}});
                    const bool unsupported_multi_draw = maximum > 1 && !session_->enabledFeatures().multi_draw_indirect;
                    const bool unsupported_count = indirect->count_buffer && !limits.features.drawIndirectCount;
                    if (unsupported_multi_draw || unsupported_count)
                        return cxx::unexpected(RenderError{kDeviceUnsupported, {p, maximum}});
                    auto arguments = buffer(*stream, indirect->arguments, EGraphUse::INDIRECT);
                    if (!arguments) return cxx::unexpected(arguments.error());
                    const auto bytes = packet.index ? sizeof(nri::DrawIndexedDesc) : sizeof(nri::DrawDesc);
                    const auto stride = indirect->stride ? indirect->stride : static_cast<std::uint32_t>(bytes);
                    const bool invalid_alignment = arguments->offset % 4 || stride % 4 || stride < bytes;
                    const bool too_small = arguments->size < bytes ||
                        std::uint64_t(maximum - 1) * stride > arguments->size - bytes;
                    if (invalid_alignment || too_small)
                        return cxx::unexpected(RenderError{kGraphInvalidRange, {p}});

                    RasterDrawPacket::Indirect command{arguments->buffer, arguments->offset, maximum, stride};
                    command.mergeable = mesh_arena;
                    if (indirect->count_buffer)
                    {
                        auto count = buffer(*stream, *indirect->count_buffer, EGraphUse::INDIRECT);
                        if (!count) return cxx::unexpected(count.error());
                        if (count->offset % 4 || count->size < sizeof(std::uint32_t))
                            return cxx::unexpected(RenderError{kGraphInvalidRange, {p}});
                        command.count_buffer = count->buffer;
                        command.count_offset = count->offset;
                    }
                    packet.draw = command;
                }
                candidate.packets.push_back(packet);
            }

            const auto readImage = [&](const nri::Texture* texture, std::uint32_t stages) noexcept
            {
                candidate.reads.push_back({texture, GraphImageRange{}, EGraphUse::SAMPLED, stages});
            };
            for (const auto& material : input.materials)
            {
                if (!used_material_programs[material.program]) continue;
                const auto& program = selected_programs[material.program];
                candidate.reads.push_back({
                    material.buffer, material.range, EGraphUse::STORAGE_READ, program.material_stages
                });
                for (const auto* texture :
                     std::span{input.material_images}.subspan(material.first_image, material.image_count))
                {
                    readImage(texture, program.shared_texture_stages);
                }
            }
            if (texture_stages)
            {
                for (const auto* texture : input.textures) readImage(texture, texture_stages);
            }
            auto& reads = candidate.reads;
            std::ranges::sort(reads, [](const auto& a, const auto& b) noexcept
            {
                if (a.physical != b.physical) return std::less<const void*>{}(a.physical, b.physical);
                if (a.usage != b.usage) return a.usage < b.usage;
                // Shared sampled reads always span the whole image.
                if (a.usage == EGraphUse::SAMPLED) return false;
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
            for (auto& read : reads)
            {
                if (read.usage != EGraphUse::SAMPLED) continue;
                const auto& desc = session_->nriCore().GetTextureDesc(*static_cast<const nri::Texture*>(read.physical));
                read.range = GraphImageRange{1, 0, desc.mipNum, 0, desc.layerNum};
            }

            if (auto sections = materializeRasterSections(slot, p, input); !sections) return sections;

            for (std::uint32_t d = 0; d < input.draws.size(); ++d)
            {
                const auto& draw = input.draws[d];
                const auto& bindings = selected_programs[draw.program.index].bindings;
                if (bindings.private_bindings.empty()) continue;
                if (auto bound = setInputs(draw, bindings); !bound) return bound;
                auto descriptors = prepareRasterDescriptorSet(slot, bindings, d, {}, inputs);
                if (!descriptors) return cxx::unexpected(descriptors.error());
            }
            // Pool growth can replace any earlier allocation. Read all final sets after allocation.
            for (std::uint32_t d = 0; d < input.draws.size(); ++d)
            {
                const auto& bindings = selected_programs[input.draws[d].program.index].bindings;
                if (bindings.private_bindings.empty()) continue;
                const auto cache = frame.raster_binding_indices.find(&bindings);
                if (cache == frame.raster_binding_indices.end())
                    return cxx::unexpected(RenderError{kFrameContract, {p, d}});
                const auto binding = cache->second.find(d);
                if (binding == cache->second.end())
                    return cxx::unexpected(RenderError{kFrameContract, {p, d}});
                auto& packet = candidate.packets[d];
                packet.first_descriptor = static_cast<std::uint32_t>(candidate.descriptors.size());
                packet.descriptor_count = 1;
                candidate.descriptors.push_back(frame.raster_bindings[binding->second].set);
            }

            if (session_->enabledFeatures().multi_draw_indirect)
                mergeIndexedIndirectDraws(candidate, session_->description().other.drawIndirectMaxNum);

            auto& resident = frame.raster_draws[command.data_index];
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
                continue;
            }
            resident.dependency_reads.swap(candidate.dependency_reads);
            std::swap(resident, candidate);
            invalidateRecording(slot, p);
        }
        return {};
    }
} // namespace lux::render::detail
