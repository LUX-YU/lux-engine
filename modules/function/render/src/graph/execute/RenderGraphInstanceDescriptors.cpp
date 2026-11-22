#include <unordered_map>
#include <lux/engine/render/commands/Error.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/device/BufferAccess.hpp>
#include <lux/engine/render/extensions/device/DescriptorTable.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/graph/execute/PassExtent.hpp>
#include <lux/engine/render/device/ImageFormat.hpp>
#include <lux/engine/render/graph/execute/FrameResources.hpp>
#include <lux/engine/render/graph/execute/RasterStreamPlan.hpp>
#include <lux/engine/render/graph/compile/ShaderBindingPlan.hpp>

namespace lux::render::detail
{
    nri::Result RenderGraphInstance::createBufferView(nri::BufferViewDesc view, nri::Descriptor*& descriptor)
        const noexcept
    {
        const auto& core = session_->nriCore();
        const auto& limits = session_->description();
        if (view.type == nri::BufferView::CONSTANT_BUFFER)
        {
            const auto alignment = limits.memoryAlignment.constantBufferOffset;
            const auto max_range = limits.memory.constantBufferMaxRange;
            const bool invalid_offset = alignment == 0 || view.offset % alignment != 0;
            const bool invalid_size = view.size == 0 || view.size > max_range;
            if (invalid_offset || invalid_size) return nri::Result::INVALID_ARGUMENT;
            if (limits.graphicsAPI == nri::GraphicsAPI::D3D12)
            {
                const auto padding = (alignment - view.size % alignment) % alignment;
                if (padding > max_range - view.size) return nri::Result::INVALID_ARGUMENT;
                view.size += padding;
            }
            const auto capacity = core.GetBufferDesc(*view.buffer).size;
            const bool outside_buffer = view.offset > capacity || view.size > capacity - view.offset;
            if (outside_buffer) return nri::Result::INVALID_ARGUMENT;
        }
        return core.CreateBufferView(view, descriptor);
    }

    namespace
    {
        constexpr auto descriptor_counts = std::array{
            &nri::DescriptorPoolDesc::descriptorSetMaxNum, &nri::DescriptorPoolDesc::samplerMaxNum,
            &nri::DescriptorPoolDesc::constantBufferMaxNum, &nri::DescriptorPoolDesc::storageStructuredBufferMaxNum,
            &nri::DescriptorPoolDesc::storageBufferMaxNum, &nri::DescriptorPoolDesc::textureMaxNum,
            &nri::DescriptorPoolDesc::storageTextureMaxNum, &nri::DescriptorPoolDesc::structuredBufferMaxNum
        };
    }

    const PassBindings& RenderGraphInstance::rasterBindings(RasterBindingKey key) const noexcept
    {
        const auto* stream = programs_->rasterStream(key.pass);
        const auto* dynamic = stream->runtime ? dynamic_raster_programs_[key.pass].get() : nullptr;
        const auto generation = dynamic ? dynamic->revision : 0;
        const auto& selected = dynamic ? dynamic->programs : stream->programs;
        if (key.generation != generation || key.program >= selected.size()) std::terminate();
        return selected[key.program].bindings;
    }

    RenderResult<void> RenderGraphInstance::prepareRasterDescriptorCapacity(std::uint32_t slot, bool resize_pool) noexcept
    {
        auto& frame = frames_[slot];
        frame.stream_descriptor_rows.resize(frame.stream_inputs.size());
        nri::DescriptorPoolDesc additional{};
        for (const auto p : plan_->binding_plan_->recordings.producer_passes)
        {
            const auto* stream = programs_->rasterStream(p);
            if (!stream) continue;
            const auto* dynamic = stream->runtime ? dynamic_raster_programs_[p].get() : nullptr;
            if (stream->runtime && !dynamic) return cxx::unexpected(RenderError{kFrameContract, {p}});
            const auto& programs = dynamic ? dynamic->programs : stream->programs;
            const auto revision = dynamic ? dynamic->revision : 0;
            const auto& input = frame.stream_inputs[p];
            auto& rows = frame.stream_descriptor_rows[p];
            rows.programs.resize(programs.size());
            rows.draws.resize(input.draws.size());
            rows.input_count = 0;
            for (auto& program : rows.programs) program.used = 0;
            for (std::size_t d = 0; d < input.draws.size(); ++d)
            {
                const auto id = input.draws[d].program;
                const bool invalid = id.stream != p || id.index >= programs.size() || id.revision != revision;
                if (invalid) return cxx::unexpected(RenderError{kFrameContract, {p}});
                const auto& bindings = programs[id.index].bindings;
                const auto& draw = input.draws[d];
                const bool valid_ranges = containsStreamRange(input.constants.size(), draw.constants) &&
                    containsStreamRange(input.vertices.size(), draw.vertices) &&
                    containsStreamRange(input.bindings.size(), draw.bindings);
                const bool valid_constants = draw.constants.count == bindings.parameter_bytes &&
                    draw.constants.first % 4 == 0;
                const auto count = draw.bindings.count;
                const bool valid_abi = valid_constants && count == bindings.uses.size();
                if (!valid_ranges || !valid_abi) return cxx::unexpected(RenderError{kFrameContract, {p}});
                if (count > frame.stream_binding_inputs.max_size() - rows.input_count)
                    return cxx::unexpected(RenderError{kDeviceCapacity});
                rows.input_count += count;
                if (bindings.private_bindings.empty()) continue;
                auto& by_program = rows.programs[id.index];
                const auto ordinal = by_program.used++;
                if (ordinal == by_program.records.size())
                {
                    if (frame.raster_bindings.size() >= kGraphInvalidIndex)
                        return cxx::unexpected(RenderError{kDeviceCapacity});
                    by_program.records.push_back(static_cast<std::uint32_t>(frame.raster_bindings.size()));
                    auto& record = frame.raster_bindings.emplace_back();
                    record.program = {p, id.index, revision};
                    record.views.resize(bindings.uses.size());
                    record.inputs.resize(bindings.uses.size());
                }
                const auto index = by_program.records[ordinal];
                rows.draws[d] = index;
                if (!frame.raster_bindings[index].set)
                {
                    if (auto counted = addPrivateDescriptors(additional, bindings); !counted) return counted;
                }
            }
        }
        if (resize_pool) return growRasterDescriptorPool(slot);
        if (!additional.descriptorSetMaxNum) return {};
        const bool fits = frame.descriptor_pool && std::ranges::all_of(descriptor_counts, [&](auto field) noexcept
        {
            return additional.*field <= frame.descriptor_capacity.*field - frame.descriptor_reserved.*field;
        });
        return fits ? RenderResult<void>{} : growRasterDescriptorPool(slot);
    }

    RenderResult<nri::DescriptorSet*> RenderGraphInstance::prepareRasterDescriptorSet(
        std::uint32_t slot, const PassBindings& bindings, std::uint32_t index,
        std::span<const RasterDescriptorInput> inputs
    ) noexcept
    {
        auto& frame = frames_[slot];
        auto& record = frame.raster_bindings[index];
        const auto& core = session_->nriCore();
        if (!record.set)
        {
            // Failed native allocation can consume pool quota. The next preflight counts that reservation.
            if (auto counted = addPrivateDescriptors(frame.descriptor_reserved, bindings); !counted)
                return cxx::unexpected(counted.error());
            nri::DescriptorSet* created = nullptr;
            const auto status = core.AllocateDescriptorSets(
                *frame.descriptor_pool, *bindings.layout, bindings.private_set, &created, 1, 0
            );
            if (status != nri::Result::SUCCESS) return cxx::unexpected(nativeError(status));
            record.set = created;
        }
        for (std::size_t i = 0; i < bindings.uses.size(); ++i)
        {
            const auto& use = bindings.uses[i];
            if (bindings.bindings[use.field].count == 0) continue;
            const auto& source = inputs[i];
            if (record.inputs[i] == source) continue;

            nri::Descriptor* descriptor = source.sampler;
            auto status = nri::Result::SUCCESS;
            if (source.image)
            {
                const auto& range = source.range;
                const auto type = bindings.bindings[use.field].image_arrayed
                    ? nri::TextureView::TEXTURE_ARRAY : nri::TextureView::TEXTURE;
                nri::TextureViewDesc view{
                    source.image, type, source.format,
                    static_cast<nri::Dim_t>(range.first_mip), static_cast<nri::Dim_t>(range.mip_count),
                    static_cast<nri::Dim_t>(range.first_layer), static_cast<nri::Dim_t>(range.layer_count)
                };
                view.planes = range.aspects == 1 ? nri::PlaneBits::COLOR :
                    range.aspects == 2 ? nri::PlaneBits::DEPTH : nri::PlaneBits::STENCIL;
                status = core.CreateTextureView(view, descriptor);
            }
            else if (source.buffer)
            {
                const auto view = bindings.bindings[use.field].bufferView(source.buffer, source.offset, source.size);
                status = createBufferView(view, descriptor);
            }
            if (status != nri::Result::SUCCESS) return cxx::unexpected(nativeError(status));
            if (!descriptor) return cxx::unexpected(RenderError{kDeviceArgument});
            const auto owned = source.sampler ? nullptr : descriptor;
            core.DestroyDescriptor(std::exchange(record.views[i], owned));
            record.inputs[i] = source;
            const nri::UpdateDescriptorRangeDesc write{record.set, use.binding, use.element, &descriptor, 1};
            core.UpdateDescriptorRanges(&write, 1);
            invalidateRecording(slot, record.program.pass);
        }
        return record.set;
    }

    RenderResult<void> RenderGraphInstance::growRasterDescriptorPool(std::uint32_t slot) noexcept
    {
        auto& frame = frames_[slot];
        const auto& core = session_->nriCore();
        auto required = plan_->binding_plan_->private_pool;
        if (auto counted = session_->descriptors().addSnapshotDescriptors(required); !counted) return counted;
        for (const auto& item : frame.raster_bindings)
        {
            if (auto counted = addPrivateDescriptors(required, rasterBindings(item.program)); !counted)
                return cxx::unexpected(counted.error());
        }
        auto capacity = frame.descriptor_capacity;
        capacity.flags |= required.flags;
        for (auto field : descriptor_counts)
        {
            if (required.*field > capacity.*field)
            {
                const auto grown = std::max<std::uint64_t>(required.*field, std::uint64_t(capacity.*field) * 2);
                capacity.*field = static_cast<std::uint32_t>(std::min<std::uint64_t>(grown, UINT32_MAX));
            }
        }
        nri::DescriptorPool* pool = nullptr;
        auto status = core.CreateDescriptorPool(session_->device(), capacity, pool);
        if (status != nri::Result::SUCCESS) return cxx::unexpected(nativeError(status));
        cxx::scope_exit rollback([&]() noexcept { core.DestroyDescriptorPool(pool); });
        auto shared = session_->descriptors().createSnapshot(*pool);
        if (!shared) return cxx::unexpected(shared.error());
        std::vector<std::pair<nri::DescriptorSet**, nri::DescriptorSet*>> replacements;
        const auto allocate = [&](std::span<const DescriptorAccess> uses, PassDescriptorBindings native,
                                  nri::DescriptorSet*& destination, const auto& view) noexcept -> RenderResult<void>
        {
            nri::DescriptorSet* set = nullptr;
            const auto allocated = core.AllocateDescriptorSets(
                *pool, *native.layout, native.private_set, &set, 1, 0
            );
            if (allocated != nri::Result::SUCCESS) return cxx::unexpected(nativeError(allocated));
            for (std::size_t i = 0; i < uses.size(); ++i)
            {
                const auto& use = uses[i];
                auto* descriptor = view(i, use);
                if (!descriptor) continue;
                const nri::UpdateDescriptorRangeDesc write{set, use.binding, use.element, &descriptor, 1};
                core.UpdateDescriptorRanges(&write, 1);
            }
            replacements.emplace_back(&destination, set);
            return {};
        };
        for (auto& pass : frame.passes)
        {
            const auto& program = plan_->binding_plan_->pass(pass.compiled_pass->binding_slot);
            if (!program.has_private_set) continue;
            const auto& native = (*programs_)[pass.compiled_pass->binding_slot];
            auto ready = allocate(program.uses, native.descriptors, pass.private_descriptors,
                [&](std::size_t i, const auto& use) noexcept -> nri::Descriptor*
            {
                if (!program.bindings[use.field].private_binding) return nullptr;
                if (use.sampler != kGraphInvalidIndex) return programs_->sampler(use.sampler);
                // Feature preparation precedes Instance::rebind. Dirty resources already
                // name the new allocation; the old cached view is not a valid replay source.
                if (frame.bindings[use.resource].dirty) return static_cast<nri::Descriptor*>(nullptr);
                return pass.history_views[i] ? pass.history_views[i] : pass.private_views[i];
            });
            if (!ready) return cxx::unexpected(ready.error());
        }
        for (auto& item : frame.raster_bindings)
        {
            const auto& program = rasterBindings(item.program);
            if (program.private_bindings.empty()) continue;
            auto ready = allocate(program.uses, program.recordBindings(), item.set,
                [&](std::size_t i, const auto& use) noexcept -> nri::Descriptor*
            {
                if (!program.bindings[use.field].count) return nullptr;
                return item.inputs[i].sampler ? item.inputs[i].sampler : item.views[i];
            });
            if (!ready) return cxx::unexpected(ready.error());
        }
        // Earlier Features may already have produced CPU packets for this same writable FIF.
        // Rewire those borrows before the old pool can retire; they have not been recorded yet.
        std::unordered_map<nri::DescriptorSet*, nri::DescriptorSet*> relocated;
        for (const auto& [destination, value] : replacements)
        {
            if (*destination) relocated.emplace(*destination, value);
            *destination = value;
        }
        const auto relocate = [&](RasterDrawList& list) noexcept
        {
            for (auto*& set : list.descriptors)
            {
                if (const auto found = relocated.find(set); found != relocated.end()) set = found->second;
            }
        };
        for (auto& list : frame.raster_draws) relocate(list);
        invalidateRecordings(slot);
        auto* old_pool = std::exchange(frame.descriptor_pool, pool);
        frame.descriptor_capacity = capacity;
        frame.descriptor_reserved = required;
        if (session_->description().graphicsAPI == nri::GraphicsAPI::D3D12)
            frame.shared_descriptors = *shared;
        rollback.release();
        if (old_pool)
        {
            session_->deferRelease(
                [core = &core, old_pool]() noexcept { core->DestroyDescriptorPool(old_pool); }
            );
        }
        return {};
    }

    void RenderGraphInstance::releaseRasterBindings(std::uint32_t pass, std::uint64_t generation) noexcept
    {
        std::vector<nri::Descriptor*> views;
        for (std::uint32_t slot = 0; slot < frames_.size(); ++slot)
        {
            auto& frame = frames_[slot];
            invalidateRecording(slot, pass);
            std::erase_if(frame.raster_bindings, [&](auto& record) noexcept
            {
                if (record.program.pass != pass || record.program.generation != generation) return false;
                views.insert(views.end(), record.views.begin(), record.views.end());
                return true;
            });
            for (auto& stream : frame.stream_descriptor_rows)
                for (auto& program : stream.programs) program.records.clear();
            for (std::uint32_t index = 0; index < frame.raster_bindings.size(); ++index)
            {
                const auto& record = frame.raster_bindings[index];
                auto& rows = frame.stream_descriptor_rows[record.program.pass].programs[record.program.program].records;
                rows.push_back(index);
            }
        }
        if (!views.empty()) session_->deferRelease([core = &session_->nriCore(), views = std::move(views)]() noexcept
        {
            for (auto* view : views) core->DestroyDescriptor(view);
        });
    }

    RenderResult<void> RenderGraphInstance::prepareFrameBindings(std::uint32_t slot) noexcept
    {
        auto& session = *session_;
        const auto& core = session.nriCore();
        const auto& plan = *plan_;
        auto& frame = frames_[slot];
        for (const auto& transition : plan.recording_.after.images)
        {
            auto barrier = transition.native;
            barrier.texture = frame.bindings[transition.resource].image;
            frame.final_images.push_back(barrier);
        }
        for (const auto& transition : plan.recording_.after.buffers)
        {
            auto barrier = transition.native;
            barrier.buffer = frame.bindings[transition.resource].buffer;
            frame.final_buffers.push_back(barrier);
        }
        auto attachmentView = [&](std::uint32_t index, GraphImageRange range, bool depth, nri::Descriptor*& result)
            -> RenderResult<void>
        {
            if (index == plan.output())
            {
                result = output_.frames[slot].view;
                return {};
            }
            if (plan.resources()[index].history != kGraphInvalidIndex)
            {
                return {}; // The selected physical History bank supplies its already-created view.
            }
            auto* image = frame.bindings[index].image;
            if (!image)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            const nri::TextureViewDesc description{
                image,
                depth ? nri::TextureView::DEPTH_STENCIL_ATTACHMENT : nri::TextureView::COLOR_ATTACHMENT,
                imageFormat(std::get<GraphImageDesc>(plan.resources()[index].description).format).native,
                static_cast<nri::Dim_t>(range.first_mip),
                1,
                static_cast<nri::Dim_t>(range.first_layer),
                static_cast<nri::Dim_t>(range.layer_count)
            };
            const auto status = core.CreateTextureView(description, result);
            if (status != nri::Result::SUCCESS)
            {
                return cxx::unexpected(nativeError(status));
            }
            frame.views.push_back(result);
            return {};
        };
        auto attachment = [&](const RasterAttachment& input, bool depth, nri::AttachmentDesc& result
                          ) -> RenderResult<void>
        {
            auto view = attachmentView(
                input.access.version.resource.index, std::get<GraphImageRange>(input.access.range),
                depth, result.descriptor
            );
            if (!view)
            {
                return view;
            }
            if (input.resolve)
            {
                auto resolved = attachmentView(
                    input.resolve->version.resource.index, std::get<GraphImageRange>(input.resolve->range),
                    depth, result.resolveDst
                );
                if (!resolved)
                {
                    return resolved;
                }
            }
            result.loadOp = input.load == EAttachmentLoad::CLEAR  ? nri::LoadOp::CLEAR
                            : input.load == EAttachmentLoad::LOAD ? nri::LoadOp::LOAD
                                                                  : nri::LoadOp::CLEAR;
            result.storeOp = input.store == EAttachmentStore::STORE ? nri::StoreOp::STORE : nri::StoreOp::DISCARD;
            if (depth)
            {
                result.clearValue.depthStencil.depth = input.clear_depth;
            }
            else
            {
                result.clearValue.color.f = {input.clear[0], input.clear[1], input.clear[2], input.clear[3]};
            }
            return {};
        };
        for (const auto& compiled_pass : plan.passes())
        {
            Pass pass{&compiled_pass};
            auto extent = validatePassExtent(
                session, plan, compiled_pass,
                [&](std::uint32_t resource) -> const Binding& { return frame.bindings[resource]; }
            );
            if (!extent)
            {
                return extent;
            }
            for (const auto& barrier : compiled_pass.before.images)
            {
                auto native = barrier.native;
                native.texture = frame.bindings[barrier.resource].image;
                if (!native.texture)
                {
                    return cxx::unexpected(RenderError{kDeviceArgument});
                }
                pass.images.push_back(native);
            }
            for (const auto& barrier : compiled_pass.before.buffers)
            {
                auto native = barrier.native;
                native.buffer = frame.bindings[barrier.resource].buffer;
                if (native.buffer && native.before.stages == nri::StageBits::ALL &&
                    native.before.access == nri::AccessBits::NONE)
                {
                    native.before.access = bufferAccess(core.GetBufferDesc(*native.buffer).usage);
                }
                pass.buffers.push_back(native);
            }
            if (const auto* raster = plan_->recordingProgram().rasterScope(compiled_pass))
            {
                const auto& group = plan.recording_.groups[compiled_pass.rendering_group];
                pass.colors.resize(group.colors.size());
                for (std::size_t i = 0; i < group.colors.size(); ++i)
                {
                    auto ready = attachment(group.colors[i], false, pass.colors[i]);
                    if (!ready)
                    {
                        return ready;
                    }
                }
                if (raster->depth)
                {
                    auto ready = attachment(*group.depth, true, pass.depth);
                    if (!ready)
                    {
                        return ready;
                    }
                }
            }
            if (const auto* transfer = plan_->transferCommands(compiled_pass))
            {
                pass.frozen_transfers.reserve(transfer->operations.size());
                pass.clear_views.resize(transfer->clear_view_count);
                for (const auto& operation : transfer->operations)
                {
                    if (const auto* clear = std::get_if<ClearPacket>(&operation))
                    {
                        const auto& image = std::get<GraphImageDesc>(
                            plan.resources()[clear->destination.slot].description
                        );
                        auto ready = attachmentView(
                            clear->destination.slot, clear->destination.range,
                            rdesc::textureAspectMask(image.format) != 1,
                            pass.clear_views[clear->view]
                        );
                        if (!ready)
                        {
                            return ready;
                        }
                    }
                }
            }
            pass.parameters =
                std::span(frame.parameters).subspan(compiled_pass.parameter_offset, compiled_pass.parameter_size);
            frame.passes.push_back(std::move(pass));
        }
        // Pass arrays are fixed now. Acquiring another image only changes these borrowed view
        // fields, without allocating descriptors or searching author names during recording.
        for (auto& pass : frame.passes)
        {
            if (const auto* raster = plan_->recordingProgram().rasterScope(*pass.compiled_pass))
            {
                for (std::size_t i = 0; i < raster->colors.size(); ++i)
                {
                    const auto& attachment = raster->colors[i];
                    if (attachment.access.version.resource.index == plan.output())
                    {
                        frame.output_views.push_back(&pass.colors[i].descriptor);
                    }
                    if (attachment.resolve && attachment.resolve->version.resource.index == plan.output())
                    {
                        frame.output_views.push_back(&pass.colors[i].resolveDst);
                    }
                }
            }
            else if (const auto* transfer = plan_->transferCommands(*pass.compiled_pass))
            {
                for (const auto& operation : transfer->operations)
                {
                    const auto* clear = std::get_if<ClearPacket>(&operation);
                    if (clear && clear->destination.slot == plan.output())
                    {
                        frame.output_views.push_back(&pass.clear_views[clear->view]);
                    }
                }
            }
        }
        return {};
    }

    RenderResult<void> RenderGraphInstance::prepareDescriptors() noexcept
    {
        const auto& layout = *plan_->binding_plan_;
        auto pool = layout.private_pool;
        if (auto counted = session_->descriptors().addSnapshotDescriptors(pool); !counted) return counted;
        for (std::uint32_t slot = 0; slot < frames_.size(); ++slot)
        {
            auto& frame = frames_[slot];
            if (pool.descriptorSetMaxNum)
            {
                const auto status = session_->nriCore().CreateDescriptorPool(*device(), pool, frame.descriptor_pool);
                if (status != nri::Result::SUCCESS)
                {
                    return cxx::unexpected(nativeError(status));
                }
            }
            frame.descriptor_capacity = frame.descriptor_reserved = pool;
            if (frame.descriptor_pool)
            {
                auto shared = session_->descriptors().createSnapshot(*frame.descriptor_pool);
                if (!shared) return cxx::unexpected(shared.error());
                if (session_->description().graphicsAPI == nri::GraphicsAPI::D3D12)
                    frame.shared_descriptors = *shared;
            }
            for (auto& pass : frame.passes)
            {
                const auto& program = layout.pass(pass.compiled_pass->binding_slot);
                pass.private_views.resize(program.uses.size());
                pass.history_views.resize(program.uses.size());
                if (program.has_private_set)
                {
                    const auto& native = (*programs_)[pass.compiled_pass->binding_slot].descriptors;
                    const auto status = session_->nriCore().AllocateDescriptorSets(
                        *frame.descriptor_pool,
                        *native.layout,
                        native.private_set,
                        &pass.private_descriptors,
                        1,
                        0
                    );
                    if (status != nri::Result::SUCCESS)
                    {
                        return cxx::unexpected(nativeError(status));
                    }
                    for (const auto& use : program.uses)
                    {
                        if (use.sampler == kGraphInvalidIndex)
                        {
                            continue;
                        }
                        auto* sampler = programs_->sampler(use.sampler);
                        const nri::UpdateDescriptorRangeDesc
                            update{pass.private_descriptors, use.binding, use.element, &sampler, 1};
                        session_->nriCore().UpdateDescriptorRanges(&update, 1);
                    }
                }
            }
            for (std::uint32_t i = 0; i < frame.bindings.size(); ++i)
            {
                auto& binding = frame.bindings[i];
                if (!binding.dirty)
                {
                    binding.dirty = true;
                    frame.changed_resources.push_back(i);
                }
            }
            if (auto rebound = rebind(slot); !rebound)
            {
                return rebound;
            }
        }
        return {};
    }

    RenderResult<void> RenderGraphInstance::rebind(std::uint32_t slot) noexcept
    {
        auto& frame = frames_[slot];
        const auto& layout = *plan_->binding_plan_;
        const auto& core = session_->nriCore();
        for (auto resource : frame.changed_resources)
        {
            auto& source = frame.bindings[resource];
            const auto& uses = plan_->resources()[resource];
            const bool image = std::holds_alternative<GraphImageDesc>(uses.description);
            if (!source.dirty)
            {
                continue;
            }
            if (!image && !source.buffer)
            {
                source.dirty = false;
                continue;
            }
            for (const auto [p, index] : uses.barrier_uses)
            {
                if (image)
                {
                    frame.passes[p].images[index].texture = source.image;
                }
                else
                {
                    auto& barrier = frame.passes[p].buffers[index];
                    barrier.buffer = source.buffer;
                    const auto& pass = *frame.passes[p].compiled_pass;
                    if (pass.before.buffers[index].bind_initial_access)
                    {
                        barrier.before.access = bufferAccess(source.buffer_description->usage);
                    }
                }
            }
            for (auto index : uses.final_barrier_uses)
            {
                if (image)
                {
                    frame.final_images[index].texture = source.image;
                }
                else
                {
                    frame.final_buffers[index].buffer = source.buffer;
                }
            }
            for (const auto [p, index] : uses.descriptor_uses)
            {
                if (uses.history != kGraphInvalidIndex)
                {
                    continue;
                }
                auto& pass = frame.passes[p];
                const auto& program = layout.pass(pass.compiled_pass->binding_slot);
                const auto& use = program.uses[index];
                nri::Descriptor* candidate = nullptr;
                auto status = nri::Result::SUCCESS;
                if (image)
                {
                    if (!source.image)
                    {
                        return cxx::unexpected(RenderError{kDeviceArgument});
                    }
                    const auto& range = std::get<GraphImageRange>(use.range);
                    nri::TextureViewDesc description{
                        source.image,
                        use.kind == EDescriptorKind::STORAGE_IMAGE
                            ? (program.bindings[use.field].image_arrayed ? nri::TextureView::STORAGE_TEXTURE_ARRAY
                                                                         : nri::TextureView::STORAGE_TEXTURE)
                            : (program.bindings[use.field].image_arrayed ? nri::TextureView::TEXTURE_ARRAY
                                                                         : nri::TextureView::TEXTURE),
                        imageFormat(std::get<GraphImageDesc>(uses.description).format).native,
                        static_cast<nri::Dim_t>(range.first_mip),
                        static_cast<nri::Dim_t>(range.mip_count),
                        static_cast<nri::Dim_t>(range.first_layer),
                        static_cast<nri::Dim_t>(range.layer_count)
                    };
                    description.planes = range.aspects == 1   ? nri::PlaneBits::COLOR
                                         : range.aspects == 2 ? nri::PlaneBits::DEPTH
                                                              : nri::PlaneBits::STENCIL;
                    status = core.CreateTextureView(description, candidate);
                }
                else
                {
                    const auto& range = std::get<GraphBufferRange>(use.range);
                    // Imported variable-size input buffers expose their current physical range.
                    const auto size =
                        use.whole_resource && use.fallback == kGraphInvalidIndex ? source.buffer_size : range.size;
                    const bool invalid_range = range.offset > source.buffer_size ||
                        size > source.buffer_size - range.offset || range.offset > UINT64_MAX - source.buffer_offset;
                    if (invalid_range)
                    {
                        return cxx::unexpected(RenderError{kDeviceArgument});
                    }
                    const auto description = program.bindings[use.field].bufferView(
                        source.buffer, source.buffer_offset + range.offset, size
                    );
                    status = createBufferView(description, candidate);
                }
                if (status != nri::Result::SUCCESS)
                {
                    return cxx::unexpected(nativeError(status));
                }
                core.DestroyDescriptor(std::exchange(pass.private_views[index], candidate));
                candidate = pass.private_views[index];
                const nri::UpdateDescriptorRangeDesc
                    update{pass.private_descriptors, use.binding, use.element, &candidate, 1};
                core.UpdateDescriptorRanges(&update, 1);
                pass.history_views[index] = nullptr;
                const auto history = plan_->resources()[use.resource].history;
                if (history != kGraphInvalidIndex) frame.pending_history_views.mark(history);
                invalidateRecording(slot, p);
            }
            source.dirty = false;
        }
        frame.changed_resources.clear();
        return {};
    }

    RenderResult<void> RenderGraphInstance::writeInlineConstant(
        std::uint32_t slot, std::uint32_t pass, std::span<const std::byte> bytes, std::uint64_t revision
    ) noexcept
    {
        if (slot >= frames_.size() || pass >= frames_[slot].passes.size())
            return cxx::unexpected(RenderError{kDeviceArgument});
        auto& selected = frames_[slot].passes[pass];
        const auto destination = selected.parameters;
        if (bytes.empty() || bytes.size() != destination.size())
            return cxx::unexpected(RenderError{kDeviceArgument});
        if (revision != 0 && revision == selected.parameter_source_revision) return {};
        selected.parameter_source_revision = revision;
        if (std::memcmp(destination.data(), bytes.data(), bytes.size()) != 0)
        {
            std::memcpy(destination.data(), bytes.data(), bytes.size());
            invalidateRecording(slot, pass);
        }
        return {};
    }
} // namespace lux::render::detail
