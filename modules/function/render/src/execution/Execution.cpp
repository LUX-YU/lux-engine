#include <algorithm>
#include <cstring>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/device/DescriptorArray.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/device/ImageFormat.hpp>
#include <lux/engine/render/execution/ExecutionContext.hpp>
#include <lux/engine/render/execution/OffscreenTarget.hpp>
#include <lux/engine/render/graph/LayoutPlan.hpp>
#include <lux/engine/render/upload/UploadContext.hpp>

namespace lux::render::detail
{
    // These are physical GPU objects, independent of the Context's subscriptions and CPU recipes.
    // Stable descriptor banks are never rewritten while their prior submission is in flight.
    struct ExecutionContext::FrameResources final
    {
        void release(VkDevice device, VmaAllocator allocator) noexcept
        {
            if (descriptor_pool)
            {
                vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
            }
            for (auto view : views)
            {
                vkDestroyImageView(device, view, nullptr);
            }
            for (auto& image : images)
            {
                image.release(allocator);
            }
            for (auto& buffer : buffers)
            {
                buffer.release(allocator);
            }
        }

        std::vector<Binding> bindings;
        std::vector<Pass> passes;
        std::vector<std::uint32_t> changed_resources;
        std::size_t unavailable_buffers = 0;
        std::vector<gapi::vk::VmaBuffer> buffers;
        std::vector<gapi::vk::VmaImage> images;
        std::vector<VkImageView> views;
        VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    };

    ExecutionContext::ExecutionContext(
        DeviceSession& session,
        const CompiledGraph& code,
        OffscreenTarget& output
    ) noexcept
        : session_(&session), code_(&code), target_(output.description()),
          storage_indices_(code.resources().size(), kGraphInvalidIndex), storages_(session.resources())
    {
        storage_uses_.reserve(code.resources().size());
        context_data_.reserve(code.contexts_.size());
        prepared_data_.reserve(code.contexts_.size());
        context_values_.resize(code.descriptor_plan_->context_parameters.size);
        parameter_copies_.resize(code.passes().size());
        for (const auto& pass : code.passes())
        {
            pass_parameters_.push_back(code.descriptor_plan_->programs[pass.layout_program].initial_parameters);
        }
        for (std::uint32_t slot = 0; slot < session.framesInFlight(); ++slot)
        {
            auto& frame = frames_[slot] = std::make_unique<FrameResources>();
            frame->bindings.resize(code.resources().size());
            frame->changed_resources.reserve(code.resources().size());
            frame->unavailable_buffers = std::ranges::count_if(
                code.resources(),
                [](const auto& resource)
                { return resource.buffer_usage && (resource.storage || resource.context_data != kGraphInvalidIndex); }
            );
            frame->images.reserve(std::ranges::count_if(
                code.resources(),
                [](const auto& resource) { return resource.image_usage && !resource.storage; }
            ));
        }
    }

    ExecutionContext::~ExecutionContext() noexcept
    {
        withdraw();
        // Physical cleanup is owned by the Session; it never calls back into this Context.
        session_->deferRelease(
            [frames = std::move(frames_), device = device(), allocator = session_->allocator().native()](
            ) mutable noexcept
            {
                for (auto& frame : frames)
                {
                    if (frame)
                    {
                        frame->release(device, allocator);
                    }
                }
            }
        );
    }

    void ExecutionContext::withdraw() noexcept
    {
        // End future-use relationships before the Attachment's actual Storage owners are destroyed.
        // Submitted physical resources remain in the unique retirement facility.
        context_data_.clear();
        for (auto& use : storage_uses_)
        {
            use.subscription.reset();
        }
        storage_uses_.clear();
        storages_.clear();
    }

    RenderResult<std::unique_ptr<ExecutionContext>> DeviceSession::prepare(
        const CompiledGraph& code,
        OffscreenTarget& output,
        StorageSet* scene,
        StorageSet* view,
        std::span<const StorageArgument> arguments
    ) noexcept
    {
        const auto& target = output.description();
        const auto max_extent = physical_device_.properties().properties.limits.maxImageDimension2D;
        const bool invalid = &output.session() != this || code.device() != nativeDevice() || !code.compatible(target) ||
                             target.width == 0 || target.height == 0 || target.width > max_extent ||
                             target.height > max_extent;
        if (invalid)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        const bool is_wrong_owner =
            (scene && &scene->registry() != &resources_) || (view && &view->registry() != &resources_);
        if (is_wrong_owner)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        std::vector<const StorageArgument*> resolved_arguments(code.resources().size());
        for (const auto& argument : arguments)
        {
            const auto plan = std::ranges::find(code.resources(), argument.resource_key, &CompiledResource::key);
            if (plan == code.resources().end())
            {
                return cxx::unexpected(RenderError{kStorageContract});
            }
            if (!plan->image_usage && !plan->buffer_usage)
            {
                continue;
            }
            if (!plan->storage)
            {
                return cxx::unexpected(RenderError{kStorageContract});
            }
            const auto index = static_cast<std::size_t>(plan - code.resources().begin());
            const auto* previous = resolved_arguments[index];
            if (previous && *previous != argument)
            {
                return cxx::unexpected(RenderError{kStorageMismatch});
            }
            resolved_arguments[index] = &argument;
        }
        std::vector<std::pair<StorageSet*, StorageId>> created_storages;
        cxx::scope_exit rollback(
            [&]() noexcept
            {
                for (const auto& [set, id] : created_storages)
                {
                    (void)set->remove(id);
                }
            }
        );
        auto context = std::unique_ptr<ExecutionContext>(new ExecutionContext(*this, code, output));
        const auto prepareGroup = [&](std::span<const std::uint32_t> indices, StorageSet* set) -> RenderResult<void>
        {
            for (auto i : indices)
            {
                const auto& plan = context->code_->resources()[i];
                if (!plan.image_usage && !plan.buffer_usage)
                {
                    continue;
                }
                if (!set)
                {
                    return cxx::unexpected(RenderError{kStorageIdentity});
                }
                const auto* factory = resources_.factory(plan.storage->type.view());
                const auto key = plan.storage->key.empty() ? plan.key : plan.storage->key;
                const auto* argument = resolved_arguments[i];
                const auto type = argument ? argument->typeInfo() : CppType{};
                if (type != factory->descriptor->binding_type)
                {
                    return cxx::unexpected(RenderError{kStorageContract});
                }
                const auto* value = argument ? argument->data() : nullptr;
                const auto storage_type = resources_.typeId(plan.storage->type.view());
                const auto previous = set->find(storage_type, key);
                auto instance = set->getOrCreate(storage_type, key, plan.storage->config);
                if (!instance)
                {
                    return cxx::unexpected(instance.error());
                }
                const auto id = *instance;
                if (previous.isNull() && set != &context->storages_)
                {
                    created_storages.emplace_back(set, id);
                }
                context->storage_indices_[i] = static_cast<std::uint32_t>(context->storage_uses_.size());
                auto& use = context->storage_uses_.emplace_back(context.get(), id, i);
                use.subscription.receiver = &use;
                use.subscription.changed = [](void* value, std::uint32_t slot, const StorageResource& resource) noexcept
                {
                    auto& link = *static_cast<ExecutionContext::StorageUse*>(value);
                    link.context->setResource(link.resource, slot, resource);
                };
                const auto& ops = *static_cast<const StorageBindingOps*>(factory->binding_ops);
                use.ops = &ops;
                use.native = resources_.native(id);
                std::array<StorageResource, 3> slices;
                auto resolved =
                    ops.resolve(use.native, plan, value, use.subscription, std::span(slices).first(frame_count_));
                if (!resolved)
                {
                    return cxx::unexpected(resolved.error());
                }
                for (std::uint32_t slot = 0; slot < frame_count_; ++slot)
                {
                    context->setResource(i, slot, slices[slot]);
                }
            }
            return {};
        };
        const auto& groups = context->code_->storageGroups();
        if (auto r = prepareGroup(groups.global, &global_storages_); !r)
        {
            return cxx::unexpected(r.error());
        }
        if (auto r = prepareGroup(groups.scene, scene); !r)
        {
            return cxx::unexpected(r.error());
        }
        if (auto r = prepareGroup(groups.view, view); !r)
        {
            return cxx::unexpected(r.error());
        }
        if (auto r = prepareGroup(groups.attachment, &context->storages_); !r)
        {
            return cxx::unexpected(r.error());
        }
        for (const auto& contribution : code.contexts_)
        {
            auto value = contribution.create(*this, *context, contribution.resource, contribution.configuration.data());
            if (!value)
            {
                return cxx::unexpected(value.error());
            }
            context->context_data_.push_back(std::move(*value));
        }
        for (std::uint32_t slot = 0; slot < frame_count_; ++slot)
        {
            auto& frame = *context->frames_[slot];
            for (std::uint32_t i = 0; i < frame.bindings.size(); ++i)
            {
                auto& binding = frame.bindings[i];
                const auto& plan = context->code_->resources()[i];
                if ((!plan.image_usage && !plan.buffer_usage) || plan.storage ||
                    plan.context_data != kGraphInvalidIndex)
                {
                    continue;
                }
                if (i == code.output())
                {
                    const auto frame = output.frame(slot);
                    binding.image = frame.image;
                    binding.view = frame.view;
                    continue;
                }
                if (const auto* image = std::get_if<GraphImageDesc>(&plan.description))
                {
                    const VkExtent2D extent{
                        plan.target_sized ? target.width : image->width,
                        plan.target_sized ? target.height : image->height
                    };
                    const auto format = imageFormat(image->format);
                    auto backing = allocator_.createImage({extent, format.native, plan.image_usage});
                    if (!backing)
                    {
                        return cxx::unexpected(backing.error());
                    }
                    frame.images.push_back(std::move(*backing));
                    binding.image = &frame.images.back();
                    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                    view.image = binding.image->handle();
                    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
                    view.format = format.native;
                    view.subresourceRange = {format.aspects, 0, 1, 0, 1};
                    const bool attachment = plan.image_usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                                                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
                    if (attachment)
                    {
                        const auto status = vkCreateImageView(nativeDevice(), &view, nullptr, &binding.view);
                        if (status != VK_SUCCESS)
                        {
                            return cxx::unexpected(nativeError(status));
                        }
                        frame.views.push_back(binding.view);
                    }
                }
                else
                {
                    const auto size = std::get<GraphBufferDesc>(plan.description).size;
                    auto buffer = allocator_.createBuffer(size, plan.buffer_usage, EMemoryAccess::DEVICE);
                    if (!buffer)
                    {
                        return cxx::unexpected(buffer.error());
                    }
                    binding.buffer = buffer->handle();
                    binding.buffer_size = size;
                    frame.buffers.push_back(std::move(*buffer));
                }
            }
            for (const auto& recipe : context->code_->passes())
            {
                ExecutionContext::Pass pass{&recipe};
                for (const auto& barrier : recipe.images)
                {
                    auto native = barrier.native;
                    native.image = frame.bindings[barrier.resource].image->handle();
                    if (native.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED)
                    {
                        // A full overwrite may discard prior frame content, but must
                        // still order previous uses of this reusable physical image.
                        native.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                        native.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
                    }
                    pass.images.push_back(native);
                }
                for (const auto& barrier : recipe.buffers)
                {
                    auto native = barrier.native;
                    native.buffer = frame.bindings[barrier.resource].buffer;
                    pass.buffers.push_back(native);
                }
                frame.passes.push_back(std::move(pass));
            }
        }
        if (auto ready = context->prepareDescriptors(); !ready)
        {
            return cxx::unexpected(ready.error());
        }
        rollback.release();
        return context;
    }

    RenderResult<void> ExecutionContext::prepareDescriptors() noexcept
    {
        const auto& layout = *code_->descriptor_plan_;
        // Actual Storage parameter offsets are qualified once at Attach. Physical slices
        // share that type/layout; only their values and GPU addresses differ at recording.
        for (std::uint32_t p = 0; p < code_->passes().size(); ++p)
        {
            const auto& recipe = code_->passes()[p];
            if (recipe.recipe != ERecipe::DRAW && recipe.recipe != ERecipe::DISPATCH)
            {
                continue;
            }
            for (const auto& mapping : layout.programs[recipe.layout_program].parameters)
            {
                const auto& source =
                    mapping.resource == kGraphInvalidIndex
                        ? StorageResource{VK_NULL_HANDLE, 0, 0, context_values_, &layout.context_parameters}
                        : StorageResource{
                              VK_NULL_HANDLE,
                              0,
                              0,
                              frames_[0]->bindings[mapping.resource].parameters,
                              frames_[0]->bindings[mapping.resource].parameter_layout
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
        DescriptorArray* shared = nullptr;
        if (!layout.shared_bindings.empty())
        {
            auto table = session_->sampledImageArray();
            if (!table)
            {
                return cxx::unexpected(table.error());
            }
            shared = *table;
        }
        const auto draws = std::ranges::count_if(
            code_->passes(),
            [](const auto& recipe) { return recipe.recipe == ERecipe::DRAW || recipe.recipe == ERecipe::DISPATCH; }
        );
        const auto descriptors = std::uint64_t(draws) * layout.private_bindings.size();
        if (std::uint64_t(draws) > UINT32_MAX || descriptors > UINT32_MAX)
        {
            return cxx::unexpected(RenderError{kCapacity});
        }
        for (std::uint32_t slot = 0; slot < session_->framesInFlight(); ++slot)
        {
            auto& frame = *frames_[slot];
            if (!layout.private_bindings.empty() && draws)
            {
                const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, std::uint32_t(descriptors)};
                VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
                pool.maxSets = std::uint32_t(draws);
                pool.poolSizeCount = 1;
                pool.pPoolSizes = &size;
                if (auto result = vkCreateDescriptorPool(device(), &pool, nullptr, &frame.descriptor_pool);
                    result != VK_SUCCESS)
                {
                    return cxx::unexpected(nativeError(result));
                }
            }
            for (auto& pass : frame.passes)
            {
                if (pass.recipe->recipe != ERecipe::DRAW && pass.recipe->recipe != ERecipe::DISPATCH)
                {
                    continue;
                }
                pass.descriptors.resize(code_->descriptor_layouts_.size());
                if (shared)
                {
                    pass.descriptors[layout.shared_set] = shared->descriptors();
                }
                if (frame.descriptor_pool)
                {
                    const auto native_layout = code_->descriptor_layouts_[layout.private_set];
                    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                    allocate.descriptorPool = frame.descriptor_pool;
                    allocate.descriptorSetCount = 1;
                    allocate.pSetLayouts = &native_layout;
                    if (auto result = vkAllocateDescriptorSets(device(), &allocate, &pass.private_descriptors);
                        result != VK_SUCCESS)
                    {
                        return cxx::unexpected(nativeError(result));
                    }
                    pass.descriptors[layout.private_set] = pass.private_descriptors;
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
            rebind(slot);
        }
        return {};
    }

    StorageId ExecutionContext::storageId(std::uint32_t resource) const noexcept
    {
        const auto index = storage_indices_[resource];
        return index == kGraphInvalidIndex ? StorageId{} : storage_uses_[index].storage_id;
    }

    VkBuffer ExecutionContext::nativeBuffer(std::uint32_t resource, std::uint32_t slot) const noexcept
    {
        return frames_[slot]->bindings[resource].buffer;
    }

    void ExecutionContext::setResource(
        std::uint32_t resource,
        std::uint32_t slot,
        const StorageResource& value
    ) noexcept
    {
        const auto first = slot == UINT32_MAX ? 0 : slot;
        const auto end = slot == UINT32_MAX ? session_->framesInFlight() : slot + 1;
        for (auto current = first; current < end; ++current)
        {
            auto& frame = *frames_[current];
            auto& binding = frame.bindings[resource];
            if (code_->resources()[resource].buffer_usage)
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

    void ExecutionContext::rebind(std::uint32_t slot) noexcept
    {
        auto& frame = *frames_[slot];
        const auto& layout = *code_->descriptor_plan_;
        for (auto resource : frame.changed_resources)
        {
            auto& source = frame.bindings[resource];
            source.dirty = false;
            // The real handle is published by replica preparation before this slice can be recorded.
            if (!source.buffer)
            {
                continue;
            }
            const auto& uses = code_->resources()[resource];
            for (const auto [p, index] : uses.barrier_uses)
            {
                auto& pass = frame.passes[p];
                const auto& recipe = pass.recipe->buffers[index].native;
                auto& barrier = pass.buffers[index];
                barrier.buffer = source.buffer;
                barrier.offset = source.buffer_offset + recipe.offset;
                barrier.size = recipe.size == VK_WHOLE_SIZE ? source.buffer_size - recipe.offset : recipe.size;
            }
            for (const auto [p, index] : uses.descriptor_uses)
            {
                auto& pass = frame.passes[p];
                const auto& use = layout.programs[pass.recipe->layout_program].uses[index];
                const VkDescriptorBufferInfo info{source.buffer, source.buffer_offset, source.buffer_size};
                VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = pass.private_descriptors;
                write.dstBinding = use.binding;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                write.pBufferInfo = &info;
                vkUpdateDescriptorSets(device(), 1, &write, 0, nullptr);
            }
        }
        frame.changed_resources.clear();
    }

    RenderResult<void> ExecutionContext::setContextParameters(std::span<const std::byte> bytes) noexcept
    {
        if (bytes.size() != context_values_.size())
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        std::ranges::copy(bytes, context_values_.begin());
        return {};
    }

    RenderResult<void> ExecutionContext::setParameters(GraphPassId id, std::span<const std::byte> bytes) noexcept
    {
        const auto index = id.index < code_->pass_indices_.size() ? code_->pass_indices_[id.index] : kGraphInvalidIndex;
        if (index == kGraphInvalidIndex || bytes.size() != pass_parameters_[index].size())
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        std::ranges::copy(bytes, pass_parameters_[index].begin());
        return {};
    }

    std::span<const std::byte> ExecutionContext::parameters(GraphPassId id) const noexcept
    {
        const auto index = id.index < code_->pass_indices_.size() ? code_->pass_indices_[id.index] : kGraphInvalidIndex;
        return index == kGraphInvalidIndex ? std::span<const std::byte>{} : pass_parameters_[index];
    }

    RenderResult<bool> ExecutionContext::prepareFrame(std::uint32_t slot, UploadContext* uploads) noexcept
    {
        prepared_data_.clear();
        if (!context_data_.empty() && !uploads)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        bool ready = true;
        for (std::uint32_t index = 0; index < context_data_.size(); ++index)
        {
            auto& data = context_data_[index];
            auto prepared = data.prepare(data.value.get(), slot, *uploads);
            if (!prepared)
            {
                return cxx::unexpected(prepared.error());
            }
            ready &= *prepared != FeatureFrameWork::EPreparation::WAITING;
            if (*prepared == FeatureFrameWork::EPreparation::RECORD)
            {
                prepared_data_.push_back(index);
            }
        }
        if (!ready)
        {
            return false;
        }
        auto& frame = *frames_[slot];
        if (frame.unavailable_buffers)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        if (!frame.changed_resources.empty())
        {
            rebind(slot);
        }
        return true;
    }

    void ExecutionContext::recordPreparation(CommandBatch& batch, std::uint32_t slot) noexcept
    {
        for (auto index : prepared_data_)
        {
            auto& data = context_data_[index];
            data.record(data.value.get(), batch.native(), slot);
        }
        prepared_data_.clear();
    }

    void ExecutionContext::record(CommandBatch& batch, std::uint32_t slot) noexcept
    {
        const auto command = batch.native();
        auto& frame = *frames_[slot];
        const auto& bindings = frame.bindings;
        for (std::size_t p = 0; p < frame.passes.size(); ++p)
        {
            const auto& pass = frame.passes[p];
            auto& parameters = pass_parameters_[p];
            for (const auto& copy : parameter_copies_[p])
            {
                const auto* source = copy.resource == kGraphInvalidIndex ? context_values_.data()
                                                                         : bindings[copy.resource].parameters.data();
                std::memcpy(parameters.data() + copy.destination_offset, source + copy.source_offset, copy.bytes);
            }
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.imageMemoryBarrierCount = static_cast<std::uint32_t>(pass.images.size());
            dependency.pImageMemoryBarriers = pass.images.data();
            dependency.bufferMemoryBarrierCount = static_cast<std::uint32_t>(pass.buffers.size());
            dependency.pBufferMemoryBarriers = pass.buffers.data();
            if (!pass.images.empty() || !pass.buffers.empty())
            {
                vkCmdPipelineBarrier2(command, &dependency);
            }
            const auto& recipe = *pass.recipe;
            const auto& destination = bindings[recipe.destination];
            switch (recipe.recipe)
            {
            case ERecipe::DRAW:
            {
                if (recipe.begin_rendering)
                {
                    VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                    if (recipe.color != kGraphInvalidIndex)
                    {
                        attachment.imageView = bindings[recipe.color].view;
                    }
                    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    attachment.loadOp = recipe.load;
                    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                    std::copy(
                        recipe.clear_color.begin(),
                        recipe.clear_color.end(),
                        attachment.clearValue.color.float32
                    );
                    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                    if (recipe.depth != kGraphInvalidIndex)
                    {
                        depth.imageView = bindings[recipe.depth].view;
                    }
                    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                    depth.loadOp = recipe.depth_load;
                    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                    depth.clearValue.depthStencil = {recipe.clear_depth, 0};
                    const auto extent = destination.image->extent();
                    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
                    rendering.renderArea = {{0, 0}, {extent.width, extent.height}};
                    rendering.layerCount = 1;
                    rendering.colorAttachmentCount = unsigned(recipe.color != kGraphInvalidIndex);
                    rendering.pColorAttachments = &attachment;
                    if (recipe.depth != kGraphInvalidIndex)
                    {
                        rendering.pDepthAttachment = &depth;
                    }
                    vkCmdBeginRendering(command, &rendering);
                    const VkViewport viewport{
                        viewport_.x * extent.width,
                        viewport_.y * extent.height,
                        viewport_.width * extent.width,
                        viewport_.height * extent.height,
                        viewport_.min_depth,
                        viewport_.max_depth
                    };
                    const VkRect2D scissor{{0, 0}, {extent.width, extent.height}};
                    vkCmdSetViewport(command, 0, 1, &viewport);
                    vkCmdSetScissor(command, 0, 1, &scissor);
                }
                if (recipe.bind_pipeline)
                {
                    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, recipe.pipeline);
                }
                if (!pass.descriptors.empty())
                {
                    vkCmdBindDescriptorSets(
                        command,
                        VK_PIPELINE_BIND_POINT_GRAPHICS,
                        code_->layout(),
                        0,
                        std::uint32_t(pass.descriptors.size()),
                        pass.descriptors.data(),
                        0,
                        nullptr
                    );
                }
                if (!parameters.empty())
                {
                    vkCmdPushConstants(
                        command,
                        code_->layout(),
                        code_->descriptor_plan_->parameter_stages,
                        0,
                        std::uint32_t(parameters.size()),
                        parameters.data()
                    );
                }
                for (const auto& source : recipe.vertices)
                {
                    const auto buffer = bindings[source.resource].buffer;
                    const auto offset = bindings[source.resource].buffer_offset + source.offset;
                    vkCmdBindVertexBuffers(command, source.binding, 1, &buffer, &offset);
                }
                if (recipe.draw != EDrawRecipe::VERTICES)
                {
                    vkCmdBindIndexBuffer(
                        command,
                        bindings[recipe.index_resource].buffer,
                        bindings[recipe.index_resource].buffer_offset + recipe.index_offset,
                        recipe.index_type
                    );
                }
                if (recipe.draw == EDrawRecipe::VERTICES)
                {
                    vkCmdDraw(command, recipe.count, recipe.instances, recipe.first, recipe.first_instance);
                }
                else if (recipe.draw == EDrawRecipe::INDEXED)
                {
                    vkCmdDrawIndexed(
                        command,
                        recipe.count,
                        recipe.instances,
                        recipe.first,
                        recipe.vertex_offset,
                        recipe.first_instance
                    );
                }
                else
                {
                    vkCmdDrawIndexedIndirect(
                        command,
                        bindings[recipe.indirect_resource].buffer,
                        bindings[recipe.indirect_resource].buffer_offset + recipe.indirect_offset,
                        recipe.count,
                        recipe.indirect_stride
                    );
                }
                if (recipe.end_rendering)
                {
                    vkCmdEndRendering(command);
                }
                break;
            }
            case ERecipe::DISPATCH:
                vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, recipe.pipeline);
                if (!pass.descriptors.empty())
                {
                    vkCmdBindDescriptorSets(
                        command,
                        VK_PIPELINE_BIND_POINT_COMPUTE,
                        code_->layout(),
                        0,
                        std::uint32_t(pass.descriptors.size()),
                        pass.descriptors.data(),
                        0,
                        nullptr
                    );
                }
                if (!parameters.empty())
                {
                    vkCmdPushConstants(
                        command,
                        code_->layout(),
                        code_->descriptor_plan_->parameter_stages,
                        0,
                        std::uint32_t(parameters.size()),
                        parameters.data()
                    );
                }
                vkCmdDispatch(command, recipe.dispatch[0], recipe.dispatch[1], recipe.dispatch[2]);
                break;
            case ERecipe::CLEAR_IMAGE:
            {
                VkClearColorValue value;
                std::copy(recipe.clear_color.begin(), recipe.clear_color.end(), value.float32);
                const auto format =
                    imageFormat(std::get<GraphImageDesc>(code_->resources()[recipe.destination].description).format);
                const VkImageSubresourceRange range{format.aspects, 0, 1, 0, 1};
                if (format.aspects == VK_IMAGE_ASPECT_DEPTH_BIT)
                {
                    const VkClearDepthStencilValue depth{recipe.clear_depth, 0};
                    vkCmdClearDepthStencilImage(
                        command,
                        destination.image->handle(),
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        &depth,
                        1,
                        &range
                    );
                }
                else
                {
                    vkCmdClearColorImage(
                        command,
                        destination.image->handle(),
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        &value,
                        1,
                        &range
                    );
                }
                break;
            }
            case ERecipe::FILL_BUFFER:
                vkCmdFillBuffer(
                    command,
                    destination.buffer,
                    destination.buffer_offset,
                    std::get<GraphBufferDesc>(code_->resources()[recipe.destination].description).size,
                    0
                );
                break;
            case ERecipe::COPY_IMAGE_BUFFER:
            {
                const auto& image = *bindings[recipe.source].image;
                VkBufferImageCopy region{};
                region.bufferOffset = destination.buffer_offset;
                region.imageSubresource = {
                    imageFormat(std::get<GraphImageDesc>(code_->resources()[recipe.source].description).format).aspects,
                    0,
                    0,
                    1
                };
                region.imageExtent = {image.extent().width, image.extent().height, 1};
                vkCmdCopyImageToBuffer(
                    command,
                    image.handle(),
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    destination.buffer,
                    1,
                    &region
                );
                break;
            }
            case ERecipe::COPY_BUFFER:
            {
                const auto source = bindings[recipe.source].buffer;
                const VkBufferCopy region{
                    bindings[recipe.source].buffer_offset,
                    destination.buffer_offset,
                    std::get<GraphBufferDesc>(code_->resources()[recipe.source].description).size
                };
                vkCmdCopyBuffer(command, source, destination.buffer, 1, &region);
                break;
            }
            }
        }
    }

    void ExecutionContext::submitted(std::uint32_t slot) noexcept
    {
        progress_.last_submitted_slot = slot;
        ++progress_.counts[static_cast<std::size_t>(EExecutionOutcome::EXECUTED)];
    }
} // namespace lux::render::detail
