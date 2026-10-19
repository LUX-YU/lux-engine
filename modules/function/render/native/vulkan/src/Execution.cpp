#include "ImageFormat.hpp"
#include "LayoutPlan.hpp"
#include <algorithm>
#include <cstring>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/native/vulkan/DescriptorArray.hpp>
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/Error.hpp>
#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>

namespace lux::render::vulkan
{
    // These are physical GPU objects, independent of the Context's subscriptions and CPU recipes.
    // Stable descriptor banks are never rewritten while their prior submission is in flight.
    struct ExecutionContext::FrameResources final
    {
        explicit FrameResources(VkDevice value) noexcept : device(value) {}

        ~FrameResources() noexcept
        {
            if (descriptor_pool)
            {
                vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
            }
            for (auto view : views)
            {
                vkDestroyImageView(device, view, nullptr);
            }
        }

        VkDevice device;
        SubmissionCallback completion;
        bool pending = false;
        std::vector<std::unique_ptr<Buffer>> buffers;
        std::vector<std::unique_ptr<Image>> images;
        std::vector<VkImageView> views;
        DescriptorArray* shared_descriptors = nullptr;
        VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    };

    ExecutionContext::ExecutionContext(
        DeviceSession& owner,
        SubmissionQueue& queue,
        const CompiledGraph& code,
        TargetDesc target
    ) noexcept
        : owner_(&owner), queue_(&queue), frame_resources_(std::make_unique<FrameResources>(code.device())),
          retirement_(owner.retirement().reserve()), code_(&code), target_(target),
          bindings_(code_->resources().size()), storages_(owner.resources())
    {
        storage_uses_.reserve(bindings_.size());
        changed_resources_.reserve(bindings_.size());
        context_data_.reserve(code_->contexts_.size());
        changed_data_.reserve(code_->contexts_.size());
        context_values_.resize(code_->descriptor_plan_->context_parameters.size);
    }

    ExecutionContext::~ExecutionContext() noexcept
    {
        withdraw();
        // The detached physical set owns the completion observer; it cannot call back into this Context.
        if (!owner_->retirement().retire(retirement_, frame_resources_))
        {
            std::terminate();
        }
    }

    void ExecutionContext::withdraw() noexcept
    {
        // End future-use relationships before the Attachment's actual Storage owners are destroyed.
        // Submitted physical resources remain in the unique retirement facility.
        context_data_.clear();
        changed_data_.clear();
        for (auto& use : storage_uses_)
        {
            use.subscription.reset();
        }
        storage_uses_.clear();
        storages_.clear();
    }

    RenderResult<std::unique_ptr<ExecutionContext>> DeviceSession::prepare(
        const CompiledGraph& code,
        const TargetDesc& target,
        StorageSet* scene,
        StorageSet* view,
        std::span<const StorageArgument> arguments
    ) noexcept
    {
        const auto max_extent = physical_device_.properties().properties.limits.maxImageDimension2D;
        const bool invalid = code.device() != nativeDevice() || !code.compatible(target) || target.width == 0 ||
                             target.height == 0 || target.width > max_extent || target.height > max_extent;
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
            const auto plan = std::ranges::find(code.resources(), argument.resource_key, &NativeResourcePlan::key);
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
        auto context = std::unique_ptr<ExecutionContext>(new ExecutionContext(*this, queue(), code, target));
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
                auto& binding = context->bindings_[i];
                const auto* factory = resources_.factory(plan.storage->type.view());
                const auto key = plan.storage->key.empty() ? plan.key : plan.storage->key;
                const auto* argument = resolved_arguments[i];
                const auto type = argument ? argument->typeInfo() : CppType{};
                if (type != factory->description->binding_type)
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
                auto& use = context->storage_uses_.emplace_back(context.get(), id, i);
                use.subscription.receiver = &use;
                use.subscription.changed = [](void* value, const StorageResource& resource) noexcept
                {
                    auto& link = *static_cast<ExecutionContext::StorageUse*>(value);
                    link.context->setResource(link.resource, resource);
                };
                const auto& ops = *static_cast<const StorageBindingOps*>(factory->binding_ops);
                auto resolved = ops.resolve(resources_.native(id), plan, value, use.subscription);
                if (!resolved)
                {
                    return cxx::unexpected(resolved.error());
                }
                // Count an initially absent Buffer once; setResource handles subsequent transitions.
                if (plan.buffer_usage)
                {
                    ++context->unavailable_buffers_;
                }
                context->setResource(i, *resolved);
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
            const auto index = static_cast<std::uint32_t>(context->context_data_.size());
            ++context->unavailable_buffers_;
            auto value =
                contribution.create(*this, *context, index, contribution.resource, contribution.configuration.data());
            if (!value)
            {
                return cxx::unexpected(value.error());
            }
            context->context_data_.push_back(std::move(*value));
            context->invalidateData(index);
        }
        for (std::uint32_t i = 0; i < context->bindings_.size(); ++i)
        {
            auto& binding = context->bindings_[i];
            const auto& plan = context->code_->resources()[i];
            if ((!plan.image_usage && !plan.buffer_usage) || plan.storage || plan.context_data != kGraphInvalidIndex)
            {
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
                binding.image = backing->get();
                context->frame_resources_->images.push_back(std::move(*backing));
                VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                view.image = binding.image->native();
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
                    context->frame_resources_->views.push_back(binding.view);
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
                binding.buffer = buffer->get();
                binding.buffer_size = size;
                context->frame_resources_->buffers.push_back(std::move(*buffer));
            }
        }
        for (const auto& recipe : context->code_->passes())
        {
            ExecutionContext::Pass pass{&recipe};
            pass.parameters = context->code_->descriptor_plan_->programs[recipe.layout_program].initial_parameters;
            for (const auto& barrier : recipe.images)
            {
                auto native = barrier.native;
                native.image = context->bindings_[barrier.resource].image->native();
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
                native.buffer = context->bindings_[barrier.resource].buffer->native();
                pass.buffers.push_back(native);
            }
            context->passes_.push_back(std::move(pass));
        }
        if (auto ready = context->prepareDescriptors(); !ready)
        {
            return cxx::unexpected(ready.error());
        }
        if (auto reserved = queue().reserve(context->frame_resources_->completion); !reserved)
        {
            return cxx::unexpected(reserved.error());
        }
        rollback.release();
        return context;
    }

    RenderResult<void> ExecutionContext::prepareDescriptors() noexcept
    {
        const auto& layout = *code_->descriptor_plan_;
        if (!layout.shared_bindings.empty())
        {
            auto table = owner_->sampledImageArray();
            if (!table)
            {
                return cxx::unexpected(table.error());
            }
            frame_resources_->shared_descriptors = std::move(*table);
        }
        const auto draws = std::ranges::count_if(
            passes_,
            [](const auto& pass)
            { return pass.recipe->recipe == ERecipe::DRAW || pass.recipe->recipe == ERecipe::DISPATCH; }
        );
        if (!layout.private_bindings.empty() && draws)
        {
            const auto sets = std::uint64_t(draws);
            const auto descriptors = sets * layout.private_bindings.size();
            if (sets > UINT32_MAX || descriptors > UINT32_MAX)
            {
                return cxx::unexpected(RenderError{kCapacity});
            }
            const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, std::uint32_t(descriptors)};
            VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            pool.maxSets = std::uint32_t(sets);
            pool.poolSizeCount = 1;
            pool.pPoolSizes = &size;
            const auto result = vkCreateDescriptorPool(device(), &pool, nullptr, &frame_resources_->descriptor_pool);
            if (result != VK_SUCCESS)
            {
                return cxx::unexpected(nativeError(result));
            }
        }
        for (auto& pass : passes_)
        {
            if (pass.recipe->recipe != ERecipe::DRAW && pass.recipe->recipe != ERecipe::DISPATCH)
            {
                continue;
            }
            pass.descriptors.resize(code_->descriptor_layouts_.size());
            if (frame_resources_->shared_descriptors)
            {
                pass.descriptors[layout.shared_set] = frame_resources_->shared_descriptors->descriptors();
            }
            if (frame_resources_->descriptor_pool)
            {
                const auto native_layout = code_->descriptor_layouts_[layout.private_set];
                VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                allocate.descriptorPool = frame_resources_->descriptor_pool;
                allocate.descriptorSetCount = 1;
                allocate.pSetLayouts = &native_layout;
                const auto result = vkAllocateDescriptorSets(device(), &allocate, &pass.private_descriptors);
                if (result != VK_SUCCESS)
                {
                    return cxx::unexpected(nativeError(result));
                }
            }
            for (const auto& mapping : layout.programs[pass.recipe->layout_program].parameters)
            {
                const rdesc::ShaderBlock* source_layout = &layout.context_parameters;
                auto source_bytes = std::span<const std::byte>(context_values_);
                if (mapping.resource != kGraphInvalidIndex)
                {
                    const auto& source = bindings_[mapping.resource];
                    if (!source.parameter_layout)
                    {
                        return cxx::unexpected(RenderError{kStorageContract});
                    }
                    source_layout = source.parameter_layout;
                    source_bytes = source.parameters;
                }
                const auto& fields = source_layout->fields;
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
                const auto size = std::uint64_t(field->rows) * 4 +
                                  (field->columns ? std::uint64_t(field->columns - 1) * field->matrix_stride : 0);
                const bool invalid_range = !size || size > UINT32_MAX ||
                                           std::uint64_t(field->offset) + size > source_bytes.size() ||
                                           std::uint64_t(destination.offset) + size > pass.parameters.size();
                if (mismatch || invalid_range)
                {
                    return cxx::unexpected(RenderError{kStorageContract});
                }
                pass.parameter_copies.push_back(
                    {mapping.resource, field->offset, destination.offset, std::uint32_t(size)}
                );
                if (mapping.resource != kGraphInvalidIndex)
                {
                    bindings_[mapping.resource].parameter_copies.emplace_back(
                        static_cast<std::uint32_t>(&pass - passes_.data()),
                        static_cast<std::uint32_t>(pass.parameter_copies.size() - 1)
                    );
                }
                std::memcpy(pass.parameters.data() + destination.offset, source_bytes.data() + field->offset, size);
            }
        }
        for (std::uint32_t p = 0; p < passes_.size(); ++p)
        {
            auto& pass = passes_[p];
            for (std::uint32_t b = 0; b < pass.recipe->buffers.size(); ++b)
            {
                bindings_[pass.recipe->buffers[b].resource].barriers.emplace_back(p, b);
            }
            if (pass.private_descriptors)
            {
                pass.descriptors[layout.private_set] = pass.private_descriptors;
                const auto& uses = layout.programs[pass.recipe->layout_program].uses;
                for (std::uint32_t d = 0; d < uses.size(); ++d)
                {
                    if (uses[d].kind == EDescriptorKind::STORAGE_BUFFER)
                    {
                        bindings_[uses[d].resource].descriptors.emplace_back(p, d);
                    }
                }
            }
        }
        for (std::uint32_t i = 0; i < bindings_.size(); ++i)
        {
            auto& binding = bindings_[i];
            if (!binding.dirty)
            {
                binding.dirty = true;
                changed_resources_.push_back(i);
            }
        }
        rebind();
        return {};
    }

    void ExecutionContext::invalidateData(std::uint32_t index) noexcept
    {
        auto& data = context_data_[index];
        if (!data.dirty)
        {
            data.dirty = true;
            changed_data_.push_back(index);
        }
    }

    StorageId ExecutionContext::storageId(std::uint32_t resource) const noexcept
    {
        const auto found = std::ranges::find(storage_uses_, resource, &StorageUse::resource);
        return found == storage_uses_.end() ? StorageId{} : found->storage_id;
    }

    void ExecutionContext::setResource(std::uint32_t resource, const StorageResource& value) noexcept
    {
        auto& binding = bindings_[resource];
        if (code_->resources()[resource].buffer_usage)
        {
            if (!binding.buffer && value.buffer)
            {
                --unavailable_buffers_;
            }
            else if (binding.buffer && !value.buffer)
            {
                ++unavailable_buffers_;
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
            changed_resources_.push_back(resource);
        }
    }

    void ExecutionContext::rebind() noexcept
    {
        const auto& layout = *code_->descriptor_plan_;
        for (auto resource : changed_resources_)
        {
            auto& source = bindings_[resource];
            source.dirty = false;
            for (const auto [p, index] : source.parameter_copies)
            {
                auto& pass = passes_[p];
                const auto& copy = pass.parameter_copies[index];
                std::memcpy(
                    pass.parameters.data() + copy.destination_offset,
                    source.parameters.data() + copy.source_offset,
                    copy.bytes
                );
            }
            for (const auto [p, index] : source.barriers)
            {
                auto& pass = passes_[p];
                const auto& recipe = pass.recipe->buffers[index].native;
                auto& barrier = pass.buffers[index];
                barrier.buffer = source.buffer->native();
                barrier.offset = source.buffer_offset + recipe.offset;
                barrier.size = recipe.size == VK_WHOLE_SIZE ? source.buffer_size - recipe.offset : recipe.size;
            }
            for (const auto [p, index] : source.descriptors)
            {
                auto& pass = passes_[p];
                const auto& use = layout.programs[pass.recipe->layout_program].uses[index];
                const VkDescriptorBufferInfo info{source.buffer->native(), source.buffer_offset, source.buffer_size};
                VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = pass.private_descriptors;
                write.dstBinding = use.binding;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                write.pBufferInfo = &info;
                vkUpdateDescriptorSets(device(), 1, &write, 0, nullptr);
            }
        }
        changed_resources_.clear();
    }

    RenderResult<void> ExecutionContext::setContextParameters(std::span<const std::byte> bytes) noexcept
    {
        if (bytes.size() != context_values_.size())
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        std::ranges::copy(bytes, context_values_.begin());
        for (auto& pass : passes_)
        {
            for (const auto& copy : pass.parameter_copies)
            {
                if (copy.resource != kGraphInvalidIndex)
                {
                    continue;
                }
                std::memcpy(
                    pass.parameters.data() + copy.destination_offset,
                    (copy.resource == kGraphInvalidIndex ? context_values_.data()
                                                         : bindings_[copy.resource].parameters.data()) +
                        copy.source_offset,
                    copy.bytes
                );
            }
        }
        return {};
    }

    RenderResult<void> ExecutionContext::setParameters(GraphPassId id, std::span<const std::byte> bytes) noexcept
    {
        const auto index = id.index < code_->pass_indices_.size() ? code_->pass_indices_[id.index] : kGraphInvalidIndex;
        if (index == kGraphInvalidIndex || bytes.size() != passes_[index].parameters.size())
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        auto* pass = &passes_[index];
        std::ranges::copy(bytes, pass->parameters.begin());
        // Updating CPU parameter bytes is safe while old recorded push commands execute. Storage fields
        // remain authoritative; this boundary only replaces the pass's own initial/dynamic values.
        for (const auto& copy : pass->parameter_copies)
        {
            std::memcpy(
                pass->parameters.data() + copy.destination_offset,
                (copy.resource == kGraphInvalidIndex ? context_values_.data()
                                                     : bindings_[copy.resource].parameters.data()) +
                    copy.source_offset,
                copy.bytes
            );
        }
        return {};
    }

    std::span<const std::byte> ExecutionContext::parameters(GraphPassId id) const noexcept
    {
        const auto index = id.index < code_->pass_indices_.size() ? code_->pass_indices_[id.index] : kGraphInvalidIndex;
        return index == kGraphInvalidIndex ? std::span<const std::byte>{} : passes_[index].parameters;
    }

    RenderResult<EExecutionOutcome> ExecutionContext::execute() noexcept
    {
        const auto outcome = [this](EExecutionOutcome value) noexcept
        {
            ++progress_.counts[static_cast<std::size_t>(value)];
            return value;
        };
        auto ready = queue_->poll();
        if (!ready)
        {
            return cxx::unexpected(ready.error());
        }
        if (frame_resources_->pending)
        {
            return outcome(EExecutionOutcome::PREVIOUS_SUBMISSION_PENDING);
        }
        for (auto index : changed_data_)
        {
            auto& data = context_data_[index];
            if (auto prepared = data.prepare(data.value.get()); !prepared)
            {
                return cxx::unexpected(prepared.error());
            }
        }
        if (unavailable_buffers_)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        if (queue_->recording())
        {
            return outcome(EExecutionOutcome::RECORDING_BUSY);
        }
        auto batch = queue_->beginAfterPoll();
        if (!batch)
        {
            if (batch.error().type == kCapacity)
            {
                return outcome(EExecutionOutcome::QUEUE_FULL);
            }
            return cxx::unexpected(batch.error());
        }
        if (!changed_resources_.empty())
        {
            rebind();
        }
        const auto command = batch->native();
        for (auto index : changed_data_)
        {
            auto& data = context_data_[index];
            data.record(data.value.get(), command);
        }
        for (const auto& pass : passes_)
        {
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
            const auto& destination = bindings_[recipe.destination];
            switch (recipe.recipe)
            {
            case ERecipe::DRAW:
            {
                if (recipe.begin_rendering)
                {
                    VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                    if (recipe.color != kGraphInvalidIndex)
                    {
                        attachment.imageView = bindings_[recipe.color].view;
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
                        depth.imageView = bindings_[recipe.depth].view;
                    }
                    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                    depth.loadOp = recipe.depth_load;
                    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                    depth.clearValue.depthStencil = {recipe.clear_depth, 0};
                    const auto extent = destination.image->extent();
                    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
                    rendering.renderArea = {{0, 0}, extent};
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
                    const VkRect2D scissor{{0, 0}, extent};
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
                if (!pass.parameters.empty())
                {
                    vkCmdPushConstants(
                        command,
                        code_->layout(),
                        code_->descriptor_plan_->parameter_stages,
                        0,
                        std::uint32_t(pass.parameters.size()),
                        pass.parameters.data()
                    );
                }
                for (const auto& source : recipe.vertices)
                {
                    const auto buffer = bindings_[source.resource].buffer->native();
                    const auto offset = bindings_[source.resource].buffer_offset + source.offset;
                    vkCmdBindVertexBuffers(command, source.binding, 1, &buffer, &offset);
                }
                if (recipe.draw != EDrawRecipe::VERTICES)
                {
                    vkCmdBindIndexBuffer(
                        command,
                        bindings_[recipe.index_resource].buffer->native(),
                        bindings_[recipe.index_resource].buffer_offset + recipe.index_offset,
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
                        bindings_[recipe.indirect_resource].buffer->native(),
                        bindings_[recipe.indirect_resource].buffer_offset + recipe.indirect_offset,
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
                if (!pass.parameters.empty())
                {
                    vkCmdPushConstants(
                        command,
                        code_->layout(),
                        code_->descriptor_plan_->parameter_stages,
                        0,
                        std::uint32_t(pass.parameters.size()),
                        pass.parameters.data()
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
                        destination.image->native(),
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
                        destination.image->native(),
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
                    destination.buffer->native(),
                    destination.buffer_offset,
                    std::get<GraphBufferDesc>(code_->resources()[recipe.destination].description).size,
                    0
                );
                break;
            case ERecipe::COPY_IMAGE_BUFFER:
            {
                const auto& image = *bindings_[recipe.source].image;
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
                    image.native(),
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    destination.buffer->native(),
                    1,
                    &region
                );
                break;
            }
            case ERecipe::COPY_BUFFER:
            {
                const auto* source = bindings_[recipe.source].buffer;
                const VkBufferCopy region{
                    bindings_[recipe.source].buffer_offset,
                    destination.buffer_offset,
                    std::get<GraphBufferDesc>(code_->resources()[recipe.source].description).size
                };
                vkCmdCopyBuffer(command, source->native(), destination.buffer->native(), 1, &region);
                break;
            }
            }
        }
        auto submitted = std::move(*batch).submit(
            frame_resources_->completion,
            [](void* value, RenderResult<void>) noexcept { static_cast<FrameResources*>(value)->pending = false; },
            frame_resources_.get()
        );
        if (!submitted)
        {
            return cxx::unexpected(submitted.error());
        }
        frame_resources_->pending = true;
        for (auto index : changed_data_)
        {
            context_data_[index].dirty = false;
        }
        changed_data_.clear();
        return outcome(EExecutionOutcome::EXECUTED);
    }

} // namespace lux::render::vulkan
