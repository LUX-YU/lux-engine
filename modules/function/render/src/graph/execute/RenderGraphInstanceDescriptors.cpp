#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_set>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/device/BufferAccess.hpp>
#include <lux/engine/render/extensions/device/DescriptorTable.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/graph/execute/PassExtent.hpp>
#include <lux/engine/render/device/ImageFormat.hpp>
#include <lux/engine/render/graph/execute/FrameResources.hpp>
#include <lux/engine/render/graph/compile/ShaderBindingPlan.hpp>

namespace lux::render::detail
{
    nri::Result RenderGraphInstance::createBufferView(nri::BufferViewDesc view, nri::Descriptor*& descriptor)
        const noexcept
    {
        const auto& core = session_->nriCore();
        const auto& limits = session_->description();
        if (view.type == nri::BufferView::CONSTANT_BUFFER && limits.graphicsAPI == nri::GraphicsAPI::D3D12)
        {
            const auto alignment = limits.memoryAlignment.constantBufferOffset;
            const bool invalid_offset = alignment == 0 || view.offset % alignment != 0;
            const bool invalid_size = view.size == 0 || view.size > limits.memory.constantBufferMaxRange;
            if (invalid_offset || invalid_size)
            {
                return nri::Result::INVALID_ARGUMENT;
            }
            view.size += (alignment - view.size % alignment) % alignment;
            const auto capacity = core.GetBufferDesc(*view.buffer).size;
            const bool outside_buffer = view.offset > capacity || view.size > capacity - view.offset;
            if (outside_buffer || view.size > limits.memory.constantBufferMaxRange)
            {
                return nri::Result::INVALID_ARGUMENT;
            }
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

    RenderResult<void>
    RenderGraphInstance::FrameResources::addDescriptors(
        nri::DescriptorPoolDesc& pool,
        const PassBindings& program
    ) noexcept
    {
        if (program.private_bindings.empty()) return {};
        if (pool.descriptorSetMaxNum == UINT32_MAX) return cxx::unexpected(RenderError{kDeviceCapacity});
        ++pool.descriptorSetMaxNum;
        for (const auto& binding : program.private_bindings)
        {
            auto* count = [&]() noexcept -> std::uint32_t*
            {
                switch (binding.kind)
                {
                case EDescriptorKind::TYPED_STORAGE_BUFFER: return &pool.storageBufferMaxNum;
                case EDescriptorKind::STORAGE_BUFFER: return binding.access == EShaderAccess::READ_ONLY ?
                    &pool.structuredBufferMaxNum : &pool.storageStructuredBufferMaxNum;
                case EDescriptorKind::UNIFORM_BUFFER: return &pool.constantBufferMaxNum;
                case EDescriptorKind::STORAGE_IMAGE: return &pool.storageTextureMaxNum;
                case EDescriptorKind::SAMPLED_IMAGE: return &pool.textureMaxNum;
                case EDescriptorKind::SAMPLER: return &pool.samplerMaxNum;
                }
                return nullptr;
            }();
            if (!count || binding.count > UINT32_MAX - *count)
                return cxx::unexpected(RenderError{kDeviceCapacity});
            *count += binding.count;
        }
        return {};
    }

    RenderResult<nri::DescriptorSet*>
    RenderGraphInstance::prepareRasterBindings(
        std::uint32_t slot,
        const PassBindings& bindings,
        std::uint64_t binding_key,
        std::span<const ResourceInput> inputs
    ) noexcept
    {
        auto& frame = frames_[slot];
        auto& indices = frame.raster_binding_indices[&bindings];
        auto found = indices.find(binding_key);
        if (found == indices.end())
        {
            const auto index = static_cast<std::uint32_t>(frame.raster_bindings.size());
            frame.raster_bindings.push_back({});
            auto& record = frame.raster_bindings.back();
            record.binding_key = binding_key;
            record.bindings = &bindings;
            record.views.resize(bindings.uses.size());
            record.inputs.resize(bindings.uses.size());
            found = indices.emplace(binding_key, index).first;
        }
        auto& record = frame.raster_bindings[found->second];
        const auto& core = session_->nriCore();
        if (!record.set)
        {
            nri::DescriptorPoolDesc additional{};
            if (auto counted = FrameResources::addDescriptors(additional, bindings); !counted)
                return cxx::unexpected(counted.error());
            const bool fits = frame.descriptor_pool && std::ranges::all_of(descriptor_counts, [&](auto field) noexcept
            {
                return additional.*field <= frame.descriptor_capacity.*field - frame.descriptor_reserved.*field;
            });
            if (fits)
            {
                // NRI does not promise to restore pool quota on failed allocation (its validation
                // backend consumes range counters before the native call). Reserve this attempt
                // until the pool is rebuilt; never reuse uncertain quota or discard prior live sets.
                for (auto field : descriptor_counts) frame.descriptor_reserved.*field += additional.*field;
                const auto status = core.AllocateDescriptorSets(
                    *frame.descriptor_pool, *bindings.layout, bindings.private_set, &record.set, 1, 0
                );
                if (status != nri::Result::SUCCESS) return cxx::unexpected(nativeError(status));
            }
            else
            {
                auto grown = growRasterDescriptorPool(slot);
                if (!grown) return cxx::unexpected(grown.error());
            }
        }
        for (std::size_t i = 0; i < bindings.uses.size(); ++i)
        {
            const auto& use = bindings.uses[i];
            if (bindings.bindings[use.field].count == 0) continue;
            const bool unsupported = use.kind != EDescriptorKind::UNIFORM_BUFFER &&
                use.kind != EDescriptorKind::STORAGE_BUFFER;
            if (unsupported || use.resource >= inputs.size()) return cxx::unexpected(RenderError{kDeviceArgument});
            const auto& source = inputs[use.resource];
            if (!source.buffer || source.size == 0) return cxx::unexpected(RenderError{kDeviceArgument});
            const auto& previous = record.inputs[i];
            const bool unchanged = previous.buffer == source.buffer && previous.offset == source.offset &&
                previous.size == source.size;
            if (unchanged) continue;
            nri::Descriptor* descriptor = nullptr;
            const auto view = bindings.bindings[use.field].bufferView(source.buffer, source.offset, source.size);
            const auto status = createBufferView(view, descriptor);
            if (status != nri::Result::SUCCESS) return cxx::unexpected(nativeError(status));
            core.DestroyDescriptor(std::exchange(record.views[i], descriptor));
            record.inputs[i] = source;
            const nri::UpdateDescriptorRangeDesc write{record.set, use.binding, use.element, &descriptor, 1};
            core.UpdateDescriptorRanges(&write, 1);
            invalidateRecordings(slot);
        }
        return record.set;
    }

    RenderResult<void> RenderGraphInstance::growRasterDescriptorPool(std::uint32_t slot) noexcept
    {
        auto& frame = frames_[slot];
        const auto& core = session_->nriCore();
        nri::DescriptorPoolDesc required{};
        if (auto counted = session_->descriptors().addSnapshotDescriptors(required); !counted) return counted;
        for (const auto& pass : frame.passes)
        {
            auto counted = FrameResources::addDescriptors(
                required, plan_->descriptor_plan_->passes[pass.compiled_pass->binding_slot]
            );
            if (!counted) return cxx::unexpected(counted.error());
        }
        for (const auto& item : frame.raster_bindings)
        {
            if (auto counted = FrameResources::addDescriptors(required, *item.bindings); !counted)
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
        const auto allocate = [&](const PassBindings& program, PassDescriptorBindings native,
                                  nri::DescriptorSet*& destination, const auto& view) noexcept -> RenderResult<void>
        {
            if (program.private_bindings.empty()) return {};
            nri::DescriptorSet* set = nullptr;
            const auto allocated = core.AllocateDescriptorSets(
                *pool, *native.layout, native.private_set, &set, 1, 0
            );
            if (allocated != nri::Result::SUCCESS) return cxx::unexpected(nativeError(allocated));
            for (std::size_t i = 0; i < program.uses.size(); ++i)
            {
                const auto& use = program.uses[i];
                if (program.bindings[use.field].count == 0) continue;
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
            const auto& program = plan_->descriptor_plan_->passes[pass.compiled_pass->binding_slot];
            const auto& native = (*programs_)[pass.compiled_pass->binding_slot];
            auto ready = allocate(program, native.descriptors, pass.private_descriptors,
                [&](std::size_t i, const auto& use) noexcept
            {
                if (use.sampler != kGraphInvalidIndex) return samplers_[use.sampler].descriptor;
                // Feature preparation precedes Instance::rebind. Dirty resources already
                // name the new allocation; the old cached view is not a valid replay source.
                if (frame.bindings[use.resource].dirty) return static_cast<nri::Descriptor*>(nullptr);
                return pass.history_views[i] ? pass.history_views[i] : pass.private_views[i];
            });
            if (!ready) return cxx::unexpected(ready.error());
        }
        for (auto& item : frame.raster_bindings)
        {
            auto ready = allocate(*item.bindings, item.bindings->recordBindings(), item.set,
                [&](std::size_t i, const auto&) noexcept { return item.views[i]; });
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
        for (auto& list : frame.draw_lists) relocate(list.draws);
        invalidateRecordings(slot);
        auto* old_pool = std::exchange(frame.descriptor_pool, pool);
        frame.descriptor_capacity = capacity;
        frame.descriptor_reserved = required;
        frame.shared_descriptors = *shared;
        frame.shared_binding_revision = 0;
        frame.shared_descriptor_counts = session_->descriptors().snapshotCounts();
        rollback.release();
        if (old_pool)
        {
            session_->deferRelease(
                [core = &core, old_pool]() noexcept { core->DestroyDescriptorPool(old_pool); }
            );
        }
        return {};
    }

    void RenderGraphInstance::releaseRasterBindings(std::span<const PassBindings* const> bindings) noexcept
    {
        if (bindings.empty()) return;
        const std::unordered_set<const PassBindings*> removed(bindings.begin(), bindings.end());
        std::vector<nri::Descriptor*> views;
        for (std::uint32_t slot = 0; slot < frames_.size(); ++slot)
        {
            auto& frame = frames_[slot];
            invalidateRecordings(slot);
            std::erase_if(frame.raster_bindings, [&](auto& record) noexcept
            {
                if (!removed.contains(record.bindings)) return false;
                views.insert(views.end(), record.views.begin(), record.views.end());
                return true;
            });
            frame.raster_binding_indices.clear();
            for (std::uint32_t index = 0; index < frame.raster_bindings.size(); ++index)
            {
                const auto& record = frame.raster_bindings[index];
                frame.raster_binding_indices[record.bindings].emplace(record.binding_key, index);
            }
        }
        if (!views.empty()) session_->deferRelease([core = &session_->nriCore(), views = std::move(views)]() noexcept
        {
            for (auto* view : views) core->DestroyDescriptor(view);
        });
    }

    RenderResult<void>
    RenderGraphInstance::bindSampler(std::uint32_t input, nri::Descriptor& sampler, bool comparison) noexcept
    {
        if (input >= samplers_.size() || samplers_[input].comparison != comparison)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        samplers_[input].descriptor = &sampler;
        return {};
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
        auto attachmentView = [&](const ResourceAccess& access, bool depth, nri::Descriptor*& result
                              ) -> RenderResult<void>
        {
            const auto index = access.version.resource.index;
            if (index == plan.output())
            {
                result = output_.frames[slot].view;
                return {};
            }
            if (plan.resources()[index].history != kGraphInvalidIndex)
            {
                return {}; // The selected physical History bank supplies its already-created view.
            }
            const auto& range = std::get<GraphImageRange>(access.range);
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
            auto view = attachmentView(input.access, depth, result.descriptor);
            if (!view)
            {
                return view;
            }
            if (input.resolve)
            {
                auto resolved = attachmentView(*input.resolve, depth, result.resolveDst);
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
            auto extent = preparePassExtent(
                session,
                plan,
                compiled_pass,
                [&](std::uint32_t resource) -> const Binding& { return frame.bindings[resource]; },
                pass.dispatch_groups,
                pass.copy_layouts
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
            if (const auto* raster = compiled_pass.rasterScope())
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
                pass.copy_layouts.resize(transfer->operations.size());
                pass.clear_views.resize(transfer->operations.size());
                for (std::size_t i = 0; i < transfer->operations.size(); ++i)
                {
                    const auto& operation = transfer->operations[i];
                    if (const auto* clear = std::get_if<ClearImage>(&operation))
                    {
                        const auto& image = std::get<GraphImageDesc>(
                            plan.resources()[clear->destination.version.resource.index].description
                        );
                        auto ready = attachmentView(
                            clear->destination,
                            rdesc::textureAspectMask(image.format) != 1,
                            pass.clear_views[i]
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
            if (const auto* raster = pass.compiled_pass->rasterScope())
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
                for (std::size_t i = 0; i < transfer->operations.size(); ++i)
                {
                    const auto* clear = std::get_if<ClearImage>(&transfer->operations[i]);
                    if (clear && clear->destination.version.resource.index == plan.output())
                    {
                        frame.output_views.push_back(&pass.clear_views[i]);
                    }
                }
            }
        }
        return {};
    }

    RenderResult<void> RenderGraphInstance::prepareDescriptors() noexcept
    {
        const auto& layout = *plan_->descriptor_plan_;
        // Actual input parameter offsets are qualified once at Attach. Physical slices
        // share that type/layout; only their values and GPU addresses differ at recording.
        for (std::uint32_t p = 0; p < plan_->passes().size(); ++p)
        {
            const auto& compiled_pass = plan_->passes()[p];
            if (const auto* raster = compiled_pass.rasterScope(); raster && raster->state_input)
            {
                const auto& input = *raster->state_input;
                const auto bytes = frames_[0].bindings[input.resource.index].parameters.size();
                if (input.offset > bytes || sizeof(RasterState) > bytes - input.offset)
                {
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                }
            }
            if ((plan_->transferCommands(compiled_pass) != nullptr))
            {
                continue;
            }
            const auto& pass_layout = layout.passes[compiled_pass.binding_slot];
            for (const auto& mapping : pass_layout.parameters)
            {
                if (mapping.resource == kGraphInvalidIndex) continue;
                const ResourceInput source{
                    nullptr, 0, 0, frames_[0].bindings[mapping.resource].parameters,
                    frames_[0].bindings[mapping.resource].parameter_layout
                };
                if (!source.parameter_layout)
                {
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                }
                const auto& fields = source.parameter_layout->fields;
                const auto field = std::ranges::find(fields, mapping.source_field, &rdesc::ShaderMember::path);
                if (field == fields.end())
                {
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                }
                const auto& destination = mapping.destination;
                const bool mismatch = field->scalar != destination.scalar || field->rows != destination.rows ||
                                      field->columns != destination.columns ||
                                      field->matrix_stride != destination.matrix_stride ||
                                      field->array_stride != destination.array_stride;
                const auto bytes = std::uint64_t(field->rows) * 4 +
                                   (field->columns ? std::uint64_t(field->columns - 1) * field->matrix_stride : 0);
                const bool invalid = !bytes || bytes > UINT32_MAX ||
                                     std::uint64_t(field->offset) + bytes > source.parameters.size() ||
                                     std::uint64_t(destination.offset) + bytes > compiled_pass.parameter_size;
                if (mismatch || invalid)
                {
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                }
                parameter_copies_[p].push_back(
                    {mapping.resource,
                     field->offset,
                     destination.offset,
                     std::uint32_t(bytes)}
                );
            }
        }
        for (std::uint32_t slot = 0; slot < frames_.size(); ++slot)
        {
            for (std::uint32_t pass = 0; pass < plan_->passes().size(); ++pass)
            {
                const auto* raster = plan_->passes()[pass].rasterScope();
                if (!layout.passes[pass].parameters.empty() ||
                    (raster && raster->state_input))
                {
                    markParameters(pass, slot);
                }
            }
        }
        nri::DescriptorPoolDesc pool{};
        if (auto counted = session_->descriptors().addSnapshotDescriptors(pool); !counted) return counted;
        for (const auto& pass : plan_->passes())
        {
            if (auto counted = FrameResources::addDescriptors(pool, layout.passes[pass.binding_slot]); !counted)
                return counted;
        }
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
                frame.shared_descriptors = *shared;
                frame.shared_binding_revision = 0;
                frame.shared_descriptor_counts = session_->descriptors().snapshotCounts();
            }
            for (auto& pass : frame.passes)
            {
                const auto& program = layout.passes[pass.compiled_pass->binding_slot];
                pass.private_views.resize(program.uses.size());
                pass.history_views.resize(program.uses.size());
                if (!program.private_bindings.empty())
                {
                    const auto status = session_->nriCore().AllocateDescriptorSets(
                        *frame.descriptor_pool,
                        *(*programs_)[pass.compiled_pass->binding_slot].descriptors.layout,
                        program.private_set,
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
                        auto* sampler = samplers_[use.sampler].descriptor;
                        if (!sampler)
                        {
                            return cxx::unexpected(RenderError{kDeviceArgument});
                        }
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
        const auto& layout = *plan_->descriptor_plan_;
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
                        barrier.before.access = bufferAccess(core.GetBufferDesc(*source.buffer).usage);
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
                const auto& program = layout.passes[pass.compiled_pass->binding_slot];
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
                    const bool invalid_range =
                        range.offset > source.buffer_size || size > source.buffer_size - range.offset;
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
                invalidateRecordings(slot);
            }
            source.dirty = false;
        }
        frame.changed_resources.clear();
        return {};
    }

    bool RenderGraphInstance::usesParameterSource(EParameterSource source) const noexcept
    {
        return std::ranges::any_of(plan_->descriptor_plan_->parameter_regions,
            [&](const auto& region) { return region.source == source && region.layout.size != 0; });
    }

    RenderResult<void> RenderGraphInstance::bindParameterSource(
        EParameterSource source, const rdesc::ShaderBlock& layout
    ) noexcept
    {
        const auto index = static_cast<std::size_t>(source);
        if (index >= source_sizes_.size() || source == EParameterSource::INSTANCE)
            return cxx::unexpected(RenderError{kDeviceArgument});
        std::vector<SourceCopy> copies;
        for (const auto& region : plan_->descriptor_plan_->parameter_regions)
        {
            if (region.source != source) continue;
            for (const auto& field : region.layout.fields)
            {
                const auto found = std::ranges::find(layout.fields, field.path, &rdesc::ShaderMember::path);
                if (found == layout.fields.end()) return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                const bool mismatch = field.scalar != found->scalar || field.rows != found->rows ||
                    field.columns != found->columns || field.array_stride != found->array_stride ||
                    field.matrix_stride != found->matrix_stride;
                const auto bytes = std::uint64_t(field.rows) * 4 +
                    (field.columns ? std::uint64_t(field.columns - 1) * field.matrix_stride : 0);
                const bool invalid_range = bytes == 0 || bytes > UINT32_MAX ||
                    std::uint64_t(found->offset) + bytes > layout.size ||
                    std::uint64_t(field.offset) + bytes > region.layout.size;
                if (mismatch || invalid_range) return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                copies.push_back({source, found->offset, region.offset + field.offset, std::uint32_t(bytes)});
            }
        }
        std::erase_if(source_copies_, [&](const auto& copy) { return copy.source == source; });
        source_copies_.insert(source_copies_.end(), copies.begin(), copies.end());
        source_sizes_[index] = layout.size;
        return {};
    }

    RenderResult<void> RenderGraphInstance::setParameterSource(
        EParameterSource source, std::span<const std::byte> bytes
    ) noexcept
    {
        const auto index = static_cast<std::size_t>(source);
        if (index >= source_sizes_.size() || bytes.size() != source_sizes_[index])
            return cxx::unexpected(RenderError{kDeviceArgument});
        for (const auto& copy : source_copies_)
        {
            if (copy.source != source) continue;
            std::memcpy(
                instance_values_.data() + copy.destination_offset, bytes.data() + copy.source_offset, copy.bytes
            );
            markInstanceParameters(copy.destination_offset, copy.bytes);
        }
        return {};
    }

    RenderResult<void> RenderGraphInstance::setInputs(
        std::span<const std::byte> parameters,
        std::span<const std::uint8_t> conditions
    ) noexcept
    {
        const auto local_bytes = plan_->descriptor_plan_->local_parameter_bytes;
        if (parameters.size() != local_bytes)
            return cxx::unexpected(RenderError{kDeviceArgument});
        if (auto valid = setConditions(conditions); !valid) return valid;
        if (local_bytes)
        {
            std::memcpy(instance_values_.data(), parameters.data(), local_bytes);
            markInstanceParameters(0, local_bytes);
        }
        return {};
    }

    void RenderGraphInstance::markParameters(std::uint32_t pass, std::uint32_t slot) noexcept
    {
        auto& frame = frames_[slot];
        if (frame.parameter_dirty[pass] == 0)
        {
            frame.parameter_updates.push_back(pass);
        }
        frame.parameter_dirty[pass] = 1;
    }

    void RenderGraphInstance::markInstanceParameters(std::uint32_t offset, std::uint32_t bytes) noexcept
    {
        for (const auto& region : plan_->descriptor_plan_->parameter_regions)
        {
            if (offset >= region.offset + region.layout.size || region.offset >= offset + bytes) continue;
            for (std::uint32_t slot = 0; slot < frames_.size(); ++slot)
            {
                for (auto pass : region.passes) markParameters(pass, slot);
            }
        }
    }

    RenderResult<void> RenderGraphInstance::prepareParameters(std::uint32_t slot) noexcept
    {
        auto& frame = frames_[slot];
        // Unversioned external borrows retain their prior live semantics. Versioned inputs and
        // owned instance defaults are copied only into the FIF slices they actually invalidated.
        for (auto resource : plan_->descriptor_plan_->parameter_resources)
        {
            if (frame.bindings[resource].parameter_revision == 0)
            {
                for (auto pass : plan_->descriptor_plan_->parameter_uses[resource])
                {
                    markParameters(pass, slot);
                }
            }
        }
        for (auto p : frame.parameter_updates)
        {
            if (const auto* raster = frame.passes[p].compiled_pass->rasterScope();
                raster && raster->state_input)
            {
                const auto& input = *raster->state_input;
                auto& state = frame.passes[p].raster_state;
                RasterState incoming;
                std::memcpy(
                    &incoming, frame.bindings[input.resource.index].parameters.data() + input.offset, sizeof(incoming)
                );
                if (incoming.viewport != state.viewport || incoming.depth_bias_constant != state.depth_bias_constant ||
                    incoming.depth_bias_slope != state.depth_bias_slope)
                {
                    state = incoming;
                    invalidateRecording(slot, p);
                }
                const bool invalid_bias =
                    !std::isfinite(state.depth_bias_constant) || !std::isfinite(state.depth_bias_slope);
                if (invalid_bias || !state.viewport.validate())
                {
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                }
            }
            auto& parameters = frame.passes[p].parameters;
            const auto& compiled = plan_->passes()[p];
            for (const auto& mapping : plan_->descriptor_plan_->passes[compiled.binding_slot].parameters)
            {
                if (mapping.resource != kGraphInvalidIndex) continue;
                const auto& field = mapping.destination;
                const auto bytes = std::size_t(field.rows) * 4 +
                    (field.columns ? std::size_t(field.columns - 1) * field.matrix_stride : 0);
                auto* destination = parameters.data() + field.offset;
                const auto* source = instance_values_.data() + mapping.source_offset;
                if (std::memcmp(destination, source, bytes) != 0)
                {
                    std::memcpy(destination, source, bytes);
                    invalidateRecording(slot, p);
                }
            }
            for (const auto& copy : parameter_copies_[p])
            {
                const auto* source = frame.bindings[copy.resource].parameters.data();
                auto* destination = parameters.data() + copy.destination_offset;
                if (std::memcmp(destination, source + copy.source_offset, copy.bytes) != 0)
                {
                    std::memcpy(destination, source + copy.source_offset, copy.bytes);
                    invalidateRecording(slot, p);
                }
            }
        }

        for (auto pass : frame.parameter_updates)
        {
            frame.parameter_dirty[pass] = 0;
        }
        frame.parameter_updates.clear();
        return {};
    }
} // namespace lux::render::detail
