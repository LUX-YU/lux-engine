#include <algorithm>
#include <cstring>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/device/BufferAccess.hpp>
#include <lux/engine/render/device/DescriptorTable.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/device/ImageFormat.hpp>
#include <lux/engine/render/execution/FrameExecution.hpp>
#include <lux/engine/render/execution/FrameResources.hpp>
#include <lux/engine/render/execution/OffscreenTarget.hpp>
#include <lux/engine/render/execution/RenderExecutor.hpp>
#include <lux/engine/render/execution/RenderGraphInstance.hpp>
#include <lux/engine/render/graph/PipelineLayoutPlan.hpp>
#include <lux/engine/render/upload/ResourceUploader.hpp>
#include <numeric>

namespace lux::render::detail
{
    RenderGraphInstance::RenderGraphInstance(
        DeviceSession& session,
        std::uint32_t frame_count,
        const RenderPlan& plan,
        OffscreenTarget* output
    ) noexcept
        : session_(&session), plan_(&plan), target_(output ? output->description() : plan.target()),
          storage_indices_(plan.resources().size(), kGraphInvalidIndex), output_(output)
    {
        storage_uses_.reserve(plan.resources().size());
        instance_values_.resize(plan.descriptor_plan_->context_parameters.size);
        parameter_copies_.resize(plan.passes().size());
        for (const auto& pass : plan.passes())
        {
            pass_parameters_.push_back(plan.descriptor_plan_->programs[pass.layout_program].initial_parameters);
        }
        for (const auto& program : plan.descriptor_plan_->programs)
        {
            for (const auto& use : program.uses)
            {
                if (use.sampler != kGraphInvalidIndex && use.sampler >= samplers_.size())
                {
                    samplers_.resize(std::size_t(use.sampler) + 1);
                }
            }
        }
        frames_.resize(frame_count);
        for (std::uint32_t slot = 0; slot < frames_.size(); ++slot)
        {
            auto& frame = frames_[slot];
            frame.bindings.resize(plan.resources().size());
            frame.changed_resources.reserve(plan.resources().size());
            frame.unavailable_buffers = std::ranges::count_if(
                plan.resources(),
                [](const auto& resource)
                {
                    return resource.memory_used && std::holds_alternative<GraphBufferDesc>(resource.description) &&
                           resource.origin == EGraphOrigin::IMPORTED;
                }
            );
            frame.images.reserve(std::ranges::count_if(
                plan.resources(),
                [](const auto& resource)
                {
                    return resource.memory_used && std::holds_alternative<GraphImageDesc>(resource.description) &&
                           resource.origin != EGraphOrigin::IMPORTED;
                }
            ));
        }
    }

    RenderGraphInstance::~RenderGraphInstance() noexcept
    {
        disconnectStorageBindings();
        // Physical cleanup is owned by the Session; it never calls back into this Context.
        session_->deferRelease(
            [frames = std::move(frames_), nri_core = &session_->nriCore()]() mutable noexcept
            {
                for (auto& frame : frames)
                {
                    frame.release(*nri_core);
                }
            }
        );
    }

    void RenderGraphInstance::disconnectStorageBindings() noexcept
    {
        // End future-use relationships before the Attachment's actual Storage owners are destroyed.
        // Submitted physical resources remain in the unique retirement facility.
        feature_bindings_.clear();
        for (auto& use : storage_uses_)
        {
            use.subscription.reset();
        }
        storage_uses_.clear();
    }

    RenderResult<void> RenderGraphInstance::bindStorage(
        std::uint32_t resource,
        StorageId id,
        const StorageBindingOps& ops,
        void* native,
        const void* argument
    ) noexcept
    {
        storage_indices_[resource] = static_cast<std::uint32_t>(storage_uses_.size());
        auto& use = storage_uses_.emplace_back(this, id, resource);
        use.subscription.receiver = &use;
        use.subscription.changed = [](void* value, std::uint32_t slot, const StorageResource& resource) noexcept
        {
            auto& link = *static_cast<RenderGraphInstance::StorageUse*>(value);
            link.instance->setResource(link.resource, slot, resource);
        };
        use.ops = &ops;
        use.native = native;
        std::array<StorageResource, 3> slices;
        auto resolved = ops.resolve(
            use.native,
            plan_->resources()[resource],
            argument,
            use.subscription,
            std::span(slices).first(frames_.size())
        );
        if (!resolved)
        {
            return cxx::unexpected(resolved.error());
        }
        for (std::uint32_t slot = 0; slot < frames_.size(); ++slot)
        {
            setResource(resource, slot, slices[slot]);
        }
        return {};
    }

    void RenderGraphInstance::addFeatureBinding(GraphResourceId resource, FeatureBinding binding) noexcept
    {
        feature_resources_.push_back(resource);
        feature_bindings_.push_back(std::move(binding));
        prepared_copies_.reserve(feature_bindings_.size());
    }

    void RenderGraphInstance::bindImage(GraphResourceId resource, std::uint32_t slot, nri::Texture& image) noexcept
    {
        auto& binding = frames_[slot].bindings[resource.index];
        binding.image = &image;
    }

    RenderResult<void> RenderGraphInstance::bindSampler(std::uint32_t input, nri::Descriptor& sampler) noexcept
    {
        if (input >= samplers_.size())
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        samplers_[input] = &sampler;
        return {};
    }

    RenderResult<void> RenderGraphInstance::initialize() noexcept
    {
        auto& session = *session_;
        const auto& plan = *plan_;
        const bool wrong_device = plan.device() != &session.device();
        const bool wrong_target = output_ && (&output_->session() != &session || !plan.compatible(target_) ||
                                              !target_.width || !target_.height);
        const bool missing_target = plan.output() != kGraphInvalidIndex && !output_;
        if (wrong_device || wrong_target || missing_target)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        for (std::uint32_t slot = 0; slot < frames_.size(); ++slot)
        {
            if (auto resources = createFrameResources(slot); !resources)
            {
                return resources;
            }
            if (auto passes = prepareFrameBindings(slot); !passes)
            {
                return passes;
            }
        }
        return prepareDescriptors();
    }

    RenderResult<void> RenderGraphInstance::createFrameResources(std::uint32_t slot) noexcept
    {
        auto& session = *session_;
        const auto& core = session.nriCore();
        const auto& plan = *plan_;
        auto& frame = frames_[slot];
        const auto max_extent = session.description().dimensions.texture2DMaxDim;
        for (std::uint32_t i = 0; i < frame.bindings.size(); ++i)
        {
            auto& binding = frame.bindings[i];
            const auto& resource = plan.resources()[i];
            if (!resource.memory_used || resource.shared_table)
            {
                continue;
            }
            if (resource.origin == EGraphOrigin::IMPORTED)
            {
                if (const auto* image = std::get_if<GraphImageDesc>(&resource.description))
                {
                    if (!binding.image)
                    {
                        return cxx::unexpected(RenderError{kDeviceArgument});
                    }
                    const auto& actual = core.GetTextureDesc(*binding.image);
                    const bool mismatch = actual.width != image->width || actual.height != image->height ||
                                          actual.mipNum != image->mips || actual.layerNum != image->layers ||
                                          actual.sampleNum != image->samples ||
                                          actual.format != imageFormat(image->format).native;
                    if (mismatch)
                    {
                        return cxx::unexpected(RenderError{kDeviceArgument});
                    }
                }
                continue;
            }
            if (i == plan.output())
            {
                binding.image = output_->frame(slot).image;
                continue;
            }
            if (const auto* image = std::get_if<GraphImageDesc>(&resource.description))
            {
                const auto dimension = [&](std::uint32_t absolute, std::uint32_t target)
                {
                    if (!resource.target_sized)
                    {
                        return std::uint64_t(absolute);
                    }
                    const auto& expression = image->extent;
                    const auto scaled = (std::uint64_t(target) * expression.numerator + expression.denominator - 1) /
                                        expression.denominator;
                    return (scaled + expression.alignment - 1) / expression.alignment * expression.alignment;
                };
                const auto width = dimension(image->width, target_.width);
                const auto height = dimension(image->height, target_.height);
                if (!width || !height || width > max_extent || height > max_extent)
                {
                    return cxx::unexpected(RenderError{kInvalidTargetExtent});
                }
                const nri::TextureDesc description{
                    nri::TextureType::TEXTURE_2D,
                    resource.image_usage,
                    imageFormat(image->format).native,
                    static_cast<nri::Dim_t>(width),
                    static_cast<nri::Dim_t>(height),
                    1,
                    static_cast<nri::Dim_t>(image->mips),
                    static_cast<nri::Dim_t>(image->layers),
                    static_cast<nri::Sample_t>(image->samples),
                    nri::SharingMode::CONCURRENT
                };
                if (slot == 0)
                {
                    if (auto valid = session.validateTexture(description); !valid)
                    {
                        return cxx::unexpected(valid.error());
                    }
                }
                const auto status =
                    core.CreatePlacedTexture(session.device(), NriDeviceHeap, description, binding.image);
                if (status != nri::Result::SUCCESS)
                {
                    return cxx::unexpected(nativeError(status));
                }
                frame.images.push_back(binding.image);
            }
            else
            {
                const auto size = std::get<GraphBufferDesc>(resource.description).size;
                const nri::BufferDesc description{size, 4, resource.buffer_usage};
                const auto status =
                    core.CreatePlacedBuffer(session.device(), NriDeviceHeap, description, binding.buffer);
                if (status != nri::Result::SUCCESS)
                {
                    return cxx::unexpected(nativeError(status));
                }
                binding.buffer_size = size;
                frame.buffers.push_back(binding.buffer);
            }
        }
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
            if (const auto* raster = std::get_if<RasterCommands>(&compiled_pass.work))
            {
                const auto& group = plan.recording_.groups[compiled_pass.rendering_group];
                const auto& reference = raster->colors.empty() ? *raster->depth : raster->colors.front();
                const auto& reference_range = std::get<GraphImageRange>(reference.access.range);
                const auto& reference_image =
                    core.GetTextureDesc(*frame.bindings[reference.access.version.resource.index].image);
                const auto matchesExtent = [&](const RasterAttachment& input)
                {
                    const auto& range = std::get<GraphImageRange>(input.access.range);
                    const auto& image = core.GetTextureDesc(*frame.bindings[input.access.version.resource.index].image);
                    return std::max(1u, std::uint32_t(image.width) >> range.first_mip) ==
                               std::max(1u, std::uint32_t(reference_image.width) >> reference_range.first_mip) &&
                           std::max(1u, std::uint32_t(image.height) >> range.first_mip) ==
                               std::max(1u, std::uint32_t(reference_image.height) >> reference_range.first_mip);
                };
                if (!std::ranges::all_of(raster->colors, matchesExtent) ||
                    (raster->depth && !matchesExtent(*raster->depth)))
                {
                    return cxx::unexpected(RenderError{kInvalidTargetExtent});
                }
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
            if (const auto* transfer = std::get_if<TransferWork>(&compiled_pass.work))
            {
                pass.copy_layouts.resize(transfer->operations.size());
                pass.clear_views.resize(transfer->operations.size());
                const auto regionValid =
                    [&](const ResourceAccess& access, const std::optional<ImageCopyRegion>& rectangle)
                {
                    if (!rectangle)
                    {
                        return true;
                    }
                    const auto& range = std::get<GraphImageRange>(access.range);
                    const auto& image = core.GetTextureDesc(*frame.bindings[access.version.resource.index].image);
                    const auto format = imageFormat(
                        std::get<GraphImageDesc>(plan.resources()[access.version.resource.index].description).format
                    );
                    const auto width = std::max(1u, std::uint32_t(image.width) >> range.first_mip);
                    const auto height = std::max(1u, std::uint32_t(image.height) >> range.first_mip);
                    const bool invalid =
                        !rectangle->width || !rectangle->height || range.mip_count != 1 ||
                        std::uint64_t(rectangle->x) + rectangle->width > width ||
                        std::uint64_t(rectangle->y) + rectangle->height > height || rectangle->x % format.block ||
                        rectangle->y % format.block ||
                        (rectangle->width % format.block && rectangle->x + rectangle->width != width) ||
                        (rectangle->height % format.block && rectangle->y + rectangle->height != height);
                    return !invalid;
                };
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
                    else if (const auto* copy = std::get_if<CopyImage>(&operation))
                    {
                        if (!regionValid(copy->source, copy->source_region) ||
                            !regionValid(copy->destination, copy->destination_region))
                        {
                            return cxx::unexpected(RenderError{kDeviceArgument});
                        }
                    }
                    else if (const auto* copy = std::get_if<CopyImageBuffer>(&operation))
                    {
                        const bool source_image = std::holds_alternative<GraphImageRange>(copy->source.range);
                        const auto& image_access = source_image ? copy->source : copy->destination;
                        const auto& buffer_access = source_image ? copy->destination : copy->source;
                        const auto& image_range = std::get<GraphImageRange>(image_access.range);
                        const auto& buffer_range = std::get<GraphBufferRange>(buffer_access.range);
                        const auto& buffer = frame.bindings[buffer_access.version.resource.index];
                        const auto& image =
                            core.GetTextureDesc(*frame.bindings[image_access.version.resource.index].image);
                        const auto format = imageFormat(
                            std::get<GraphImageDesc>(plan.resources()[image_access.version.resource.index].description)
                                .format
                        );
                        if (!regionValid(image_access, copy->image_region))
                        {
                            return cxx::unexpected(RenderError{kDeviceArgument});
                        }
                        const auto width = copy->image_region
                                               ? copy->image_region->width
                                               : std::max(1u, std::uint32_t(image.width) >> image_range.first_mip);
                        const auto height = copy->image_region
                                                ? copy->image_region->height
                                                : std::max(1u, std::uint32_t(image.height) >> image_range.first_mip);
                        const auto row_bytes = ((width + format.block - 1) / format.block) * format.bytes;
                        const auto rows = (height + format.block - 1) / format.block;
                        const auto& alignment = session.description().memoryAlignment;
                        const auto row = copy->row_pitch
                                             ? copy->row_pitch
                                             : (row_bytes + alignment.uploadBufferTextureRow - 1) /
                                                   alignment.uploadBufferTextureRow * alignment.uploadBufferTextureRow;
                        const auto slice = copy->slice_pitch
                                               ? copy->slice_pitch
                                               : (std::uint64_t(row) * rows + alignment.uploadBufferTextureSlice - 1) /
                                                     alignment.uploadBufferTextureSlice *
                                                     alignment.uploadBufferTextureSlice;
                        const bool invalid =
                            image_range.mip_count != 1 || image_range.layer_count != 1 || row < row_bytes ||
                            row % alignment.uploadBufferTextureRow || slice < std::uint64_t(row) * rows ||
                            slice > UINT32_MAX || slice % alignment.uploadBufferTextureSlice ||
                            slice > buffer_range.size ||
                            (buffer.buffer_offset + buffer_range.offset) % alignment.uploadBufferTextureSlice;
                        if (invalid)
                        {
                            return cxx::unexpected(RenderError{kDeviceArgument});
                        }
                        pass.copy_layouts[i] = {buffer_range.offset, row, static_cast<std::uint32_t>(slice)};
                    }
                }
            }
            frame.passes.push_back(std::move(pass));
        }
        return {};
    }

    RenderResult<void> RenderGraphInstance::prepareDescriptors() noexcept
    {
        const auto& layout = *plan_->descriptor_plan_;
        // Actual Storage parameter offsets are qualified once at Attach. Physical slices
        // share that type/layout; only their values and GPU addresses differ at recording.
        for (std::uint32_t p = 0; p < plan_->passes().size(); ++p)
        {
            const auto& compiled_pass = plan_->passes()[p];
            if (std::holds_alternative<TransferWork>(compiled_pass.work))
            {
                continue;
            }
            for (const auto& mapping : layout.programs[compiled_pass.layout_program].parameters)
            {
                const auto& source = mapping.resource == kGraphInvalidIndex
                                         ? StorageResource{nullptr, 0, 0, instance_values_, &layout.context_parameters}
                                         : StorageResource{
                                               nullptr,
                                               0,
                                               0,
                                               frames_[0].bindings[mapping.resource].parameters,
                                               frames_[0].bindings[mapping.resource].parameter_layout
                                           };
                if (!source.parameter_layout)
                {
                    return cxx::unexpected(RenderError{kStorageContract});
                }
                const auto& fields = source.parameter_layout->fields;
                const auto field = std::ranges::find(fields, mapping.source_field, &rdesc::ShaderMember::path);
                if (field == fields.end())
                {
                    return cxx::unexpected(RenderError{kStorageContract});
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
                                     std::uint64_t(destination.offset) + bytes > pass_parameters_[p].size();
                if (mismatch || invalid)
                {
                    return cxx::unexpected(RenderError{kStorageContract});
                }
                parameter_copies_[p].push_back(
                    {mapping.resource, field->offset, destination.offset, std::uint32_t(bytes)}
                );
            }
        }
        nri::DescriptorPoolDesc pool{};
        for (const auto& pass : plan_->passes())
        {
            const auto& program = layout.programs[pass.layout_program];
            if (!program.private_bindings.empty())
            {
                ++pool.descriptorSetMaxNum;
            }
            for (const auto& binding : program.private_bindings)
            {
                switch (binding.kind)
                {
                case EDescriptorKind::TYPED_STORAGE_BUFFER:
                    pool.storageBufferMaxNum += binding.count;
                    break;
                case EDescriptorKind::STORAGE_BUFFER:
                    pool.storageStructuredBufferMaxNum += binding.count;
                    break;
                case EDescriptorKind::UNIFORM_BUFFER:
                    pool.constantBufferMaxNum += binding.count;
                    break;
                case EDescriptorKind::STORAGE_IMAGE:
                    pool.storageTextureMaxNum += binding.count;
                    break;
                case EDescriptorKind::SAMPLED_IMAGE_ARRAY:
                    pool.textureMaxNum += binding.count;
                    break;
                case EDescriptorKind::SAMPLER_ARRAY:
                    pool.samplerMaxNum += binding.count;
                    break;
                }
            }
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
            for (auto& pass : frame.passes)
            {
                const auto& program = layout.programs[pass.compiled_pass->layout_program];
                pass.descriptors.resize(unsigned(program.shared) + unsigned(!program.private_bindings.empty()));
                pass.private_views.resize(program.uses.size());
                if (program.shared)
                {
                    pass.descriptors[0] = session_->descriptors().descriptors();
                }
                if (!program.private_bindings.empty())
                {
                    const auto status = session_->nriCore().AllocateDescriptorSets(
                        *frame.descriptor_pool,
                        *program.layout,
                        program.private_set,
                        &pass.private_descriptors,
                        1,
                        0
                    );
                    if (status != nri::Result::SUCCESS)
                    {
                        return cxx::unexpected(nativeError(status));
                    }
                    pass.descriptors[program.private_set] = pass.private_descriptors;
                    for (const auto& use : program.uses)
                    {
                        if (use.sampler == kGraphInvalidIndex)
                        {
                            continue;
                        }
                        auto* sampler = samplers_[use.sampler];
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

    bool RenderGraphInstance::dumpResources(DumpSink sink) const noexcept
    {
        if (!sink.write)
        {
            return false;
        }
        std::string output = "{\"allocation\":\"independent\",\"frames\":[";
        for (std::size_t i = 0; i < frames_.size(); ++i)
        {
            if (i)
            {
                output += ',';
            }
            const auto& frame = frames_[i];
            std::uint64_t bytes = 0;
            for (auto* buffer : frame.buffers)
            {
                nri::MemoryDesc memory{};
                session_->nriCore().GetBufferMemoryDesc(*buffer, nri::MemoryLocation::DEVICE, memory);
                bytes += memory.size;
            }
            for (auto* image : frame.images)
            {
                nri::MemoryDesc memory{};
                session_->nriCore().GetTextureMemoryDesc(*image, nri::MemoryLocation::DEVICE, memory);
                bytes += memory.size;
            }
            output += "{\"slot\":" + std::to_string(i) + ",\"owned_buffers\":" + std::to_string(frame.buffers.size()) +
                      ",\"owned_textures\":" + std::to_string(frame.images.size()) +
                      ",\"allocation_requirement_bytes\":" + std::to_string(bytes) + "}";
        }
        output += "],\"excludes\":\"borrowed Target/Storage, allocator pool reserve and descriptors\"}";
        return sink.write(sink.destination, output);
    }

    StorageId RenderGraphInstance::storageId(std::uint32_t resource) const noexcept
    {
        const auto index = storage_indices_[resource];
        return index == kGraphInvalidIndex ? StorageId{} : storage_uses_[index].storage_id;
    }

    nri::Buffer* RenderGraphInstance::nativeBuffer(std::uint32_t resource, std::uint32_t slot) const noexcept
    {
        return frames_[slot].bindings[resource].buffer;
    }

    void RenderGraphInstance::setResource(
        std::uint32_t resource,
        std::uint32_t slot,
        const StorageResource& value
    ) noexcept
    {
        const auto first = slot == UINT32_MAX ? 0 : slot;
        const auto end = slot == UINT32_MAX ? static_cast<std::uint32_t>(frames_.size()) : slot + 1;
        for (auto current = first; current < end; ++current)
        {
            auto& frame = frames_[current];
            auto& binding = frame.bindings[resource];
            if (plan_->resources()[resource].memory_used &&
                std::holds_alternative<GraphBufferDesc>(plan_->resources()[resource].description))
            {
                if (!binding.buffer && value.buffer)
                {
                    --frame.unavailable_buffers;
                }
                else if (binding.buffer && !value.buffer)
                {
                    ++frame.unavailable_buffers;
                }
            }
            binding.buffer = value.buffer;
            binding.buffer_offset = value.offset;
            binding.buffer_size = value.size;
            binding.parameters = value.parameters;
            binding.parameter_layout = value.parameter_layout;
            if (!binding.dirty)
            {
                binding.dirty = true;
                frame.changed_resources.push_back(resource);
            }
        }
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
                    if (barrier.before.stages == nri::StageBits::ALL && barrier.before.access == nri::AccessBits::NONE)
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
                auto& pass = frame.passes[p];
                const auto& program = layout.programs[pass.compiled_pass->layout_program];
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
                        use.kind == EDescriptorKind::STORAGE_IMAGE ? nri::TextureView::STORAGE_TEXTURE
                                                                   : nri::TextureView::TEXTURE,
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
                    // Imported variable-size Storage buffers expose their current physical range.
                    const auto size = use.whole_resource ? source.buffer_size : range.size;
                    const bool invalid_range =
                        range.offset > source.buffer_size || size > source.buffer_size - range.offset;
                    if (invalid_range)
                    {
                        return cxx::unexpected(RenderError{kDeviceArgument});
                    }
                    const nri::BufferViewDesc description{
                        source.buffer,
                        use.kind == EDescriptorKind::UNIFORM_BUFFER ? nri::BufferView::CONSTANT_BUFFER
                        : use.kind == EDescriptorKind::TYPED_STORAGE_BUFFER
                            ? nri::BufferView::STORAGE_BUFFER
                            : nri::BufferView::STORAGE_BYTE_ADDRESS_BUFFER,
                        source.buffer_offset + range.offset,
                        size,
                        use.kind == EDescriptorKind::TYPED_STORAGE_BUFFER ? nri::Format::R32_UINT : nri::Format::UNKNOWN
                    };
                    status = core.CreateBufferView(description, candidate);
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
            }
            source.dirty = false;
        }
        frame.changed_resources.clear();
        return {};
    }

    RenderResult<void> RenderGraphInstance::setInstanceParameters(std::span<const std::byte> bytes) noexcept
    {
        if (bytes.size() != instance_values_.size())
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        std::ranges::copy(bytes, instance_values_.begin());
        return {};
    }

    RenderResult<void> RenderGraphInstance::setParameters(GraphPassId id, std::span<const std::byte> bytes) noexcept
    {
        const auto index = id.index < plan_->pass_indices_.size() ? plan_->pass_indices_[id.index] : kGraphInvalidIndex;
        if (index == kGraphInvalidIndex || bytes.size() != pass_parameters_[index].size())
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        std::ranges::copy(bytes, pass_parameters_[index].begin());
        return {};
    }

    std::span<const std::byte> RenderGraphInstance::parameters(GraphPassId id) const noexcept
    {
        const auto index = id.index < plan_->pass_indices_.size() ? plan_->pass_indices_[id.index] : kGraphInvalidIndex;
        return index == kGraphInvalidIndex ? std::span<const std::byte>{} : pass_parameters_[index];
    }

    RenderResult<bool> RenderGraphInstance::prepareFrame(std::uint32_t slot, ResourceUploader* uploads) noexcept
    {
        prepared_copies_.clear();
        if (!feature_bindings_.empty() && !uploads)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        bool ready = true;
        for (std::uint32_t index = 0; index < feature_bindings_.size(); ++index)
        {
            auto& data = feature_bindings_[index];
            auto prepared = data.prepare(data.value.get(), slot, *uploads);
            if (!prepared)
            {
                return cxx::unexpected(prepared.error());
            }
            if (prepared->changed_resource)
            {
                setResource(feature_resources_[index].index, slot, *prepared->changed_resource);
            }
            ready &= prepared->state != FeatureBinding::EPreparation::WAITING;
            if (prepared->state == FeatureBinding::EPreparation::RECORD)
            {
                prepared_copies_.push_back(prepared->copies);
            }
        }
        if (!ready)
        {
            return false;
        }
        auto& frame = frames_[slot];
        if (frame.unavailable_buffers)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        if (!frame.changed_resources.empty())
        {
            if (auto rebound = rebind(slot); !rebound)
            {
                return cxx::unexpected(rebound.error());
            }
        }
        return true;
    }

} // namespace lux::render::detail
