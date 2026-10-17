#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>
#include "Native.hpp"
#include "LayoutPlan.hpp"
#include <lux/engine/render/native/vulkan/DescriptorArray.hpp>
#include <algorithm>

namespace lux::render::vulkan
{
    ExecutionContext::ExecutionContext(DeviceSession& owner, SubmissionQueue& queue,
        std::shared_ptr<const CompiledGraph> code, TargetDesc target) noexcept
        : owner_(&owner), queue_(&queue), code_(std::move(code)), target_(target),
          bindings_(code_->resources().size()), storages_(owner.resources())
    {
        leases_.reserve(bindings_.size());
    }

    ExecutionContext::~ExecutionContext() noexcept
    {
        withdraw();
        if (descriptor_pool_) LUX_DESTROY("context_descriptor_pool", vkDestroyDescriptorPool(device(), descriptor_pool_, nullptr));
        for (auto& binding : bindings_)
        {
            if (binding.view) LUX_DESTROY("image_view", vkDestroyImageView(device(), binding.view, nullptr));
            if (binding.image) owner_->destroyFinished(binding.image);
            if (binding.owned_buffer) owner_->destroyFinished(binding.owned_buffer);
        }
    }

    void ExecutionContext::withdraw() noexcept
    {
        cancelReadbacks();
        // All future-use subscriptions end while the actual Storage owners still exist. Physical
        // backing retirement is the professional Native object's responsibility, using submitted LastUse.
        for (auto& use : leases_)
        {
            use.binding.updated = nullptr;
            use.binding.reference.reset();
        }
        leases_.clear();
        storages_.clear();
    }

    RenderResult<ExecutionContext*> DeviceSession::prepare(std::shared_ptr<const CompiledGraph> code,
        const TargetDesc& target, StorageSet* scene, StorageSet* view,
        std::span<const StorageArgument> arguments) noexcept
    {
        const auto max_extent = device_.properties().limits.maxImageDimension2D;
        const bool invalid = !code || code->device() != device_.native() || !code->compatible(target) ||
            target.width == 0 || target.height == 0 || target.width > max_extent || target.height > max_extent;
        if (invalid) return cxx::unexpected(RenderError{kInvalidArgument});
        const bool is_wrong_owner = (scene && &scene->registry() != &resources_) ||
            (view && &view->registry() != &resources_);
        if (is_wrong_owner) return cxx::unexpected(RenderError{kStorageIdentity});
        std::vector<const ConfigValue*> resolved_arguments(code->resources().size());
        for (const auto& argument : arguments)
        {
            const auto plan = std::ranges::find(code->resources(), argument.resource_key, &NativeResourcePlan::key);
            if (plan == code->resources().end()) return cxx::unexpected(RenderError{kStorageContract});
            if (!plan->image_usage && !plan->buffer_usage) continue;
            if (!plan->storage) return cxx::unexpected(RenderError{kStorageContract});
            const auto index = static_cast<std::size_t>(plan - code->resources().begin());
            const auto* previous = resolved_arguments[index];
            if (previous && *previous != argument.value) return cxx::unexpected(RenderError{kStorageMismatch});
            resolved_arguments[index] = &argument.value;
        }
        auto reservation = retirement().reserve();
        if (!reservation) return cxx::unexpected(reservation.error());
        auto context = std::unique_ptr<ExecutionContext>(new ExecutionContext(*this, queue(), std::move(code), target));
        context->retirement_ = std::move(*reservation);
        const auto prepareGroup = [&](std::span<const std::uint32_t> indices, StorageSet* set) -> RenderResult<void> {
            for (auto i : indices)
            {
                const auto& plan = context->code_->resources()[i];
                if (!plan.image_usage && !plan.buffer_usage) continue;
                if (!set) return cxx::unexpected(RenderError{kStorageIdentity});
                auto& binding = context->bindings_[i];
                const auto* factory = resources_.factory(plan.storage->type.view());
                const auto key = plan.storage->key.empty() ? plan.key : plan.storage->key;
                const auto* argument = resolved_arguments[i];
                const auto type = argument ? argument->typeInfo() : ConfigValue::Type{};
                if (type != factory->description->binding_type)
                    return cxx::unexpected(RenderError{kStorageContract});
                const auto* value = argument ? argument->data() : nullptr;
                auto instance = set->acquire(resources_.typeId(plan.storage->type.view()), key, plan.storage->config);
                if (!instance) return cxx::unexpected(instance.error());
                const auto id = instance->id();
                auto& lease = context->leases_.emplace_back(std::move(*instance), PreparedStorageBinding{}, i);
                const auto& ops = *static_cast<const StorageBindingOps*>(factory->binding_ops);
                if (auto prepared = ops.prepare(resources_.native(id), plan, value, lease.binding); !prepared)
                    return cxx::unexpected(prepared.error());
                binding.buffer = lease.binding.buffer;
            }
            return {};
        };
        const auto& groups = context->code_->storageGroups();
        if (auto r = prepareGroup(groups.global, &globals_); !r) return cxx::unexpected(r.error());
        if (auto r = prepareGroup(groups.scene, scene); !r) return cxx::unexpected(r.error());
        if (auto r = prepareGroup(groups.view, view); !r) return cxx::unexpected(r.error());
        if (auto r = prepareGroup(groups.attachment, &context->storages_); !r) return cxx::unexpected(r.error());
        for (std::uint32_t i = 0; i < context->bindings_.size(); ++i)
        {
            auto& binding = context->bindings_[i];
            const auto& plan = context->code_->resources()[i];
            if ((!plan.image_usage && !plan.buffer_usage) || plan.storage) continue;
            if (const auto* image = std::get_if<GraphImageDesc>(&plan.description))
            {
                const VkExtent2D extent{plan.target_sized ? target.width : image->width,
                    plan.target_sized ? target.height : image->height};
                auto backing = createImageRecord({extent, VK_FORMAT_R8G8B8A8_UNORM, plan.image_usage});
                if (!backing) return cxx::unexpected(backing.error());
                binding.image = *backing;
                VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                view.image = binding.image->native();
                view.viewType = VK_IMAGE_VIEW_TYPE_2D;
                view.format = VK_FORMAT_R8G8B8A8_UNORM;
                view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                if (plan.image_usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
                {
                    const auto status = LUX_NATIVE("image_view", vkCreateImageView(device_.native(), &view,
                        nullptr, &binding.view));
                    if (status != VK_SUCCESS) return cxx::unexpected(nativeError(status));
                }
            }
            else
            {
                const auto size = plan.target_sized ? std::uint64_t(target.width) * target.height * 4 :
                    std::get<GraphBufferDesc>(plan.description).size;
                auto buffer = createBufferRecord(size, plan.buffer_usage,
                    plan.readback ? EMemoryAccess::READBACK : EMemoryAccess::DEVICE);
                if (!buffer) return cxx::unexpected(buffer.error());
                binding.owned_buffer = *buffer;
                binding.buffer = binding.owned_buffer;
                if (plan.readback && plan.target_sized) context->readback_ = i;
            }
        }
        for (const auto& recipe : context->code_->passes())
        {
            ExecutionContext::Pass pass{&recipe};
            pass.parameters = &context->color_;
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
        for (const auto& barrier : context->code_->hostBarriers())
        {
            auto native = barrier.native;
            native.buffer = context->bindings_[barrier.resource].buffer->native();
            context->host_barriers_.push_back(native);
        }
        if (auto ready = context->prepareDescriptors(); !ready) return cxx::unexpected(ready.error());
        for (auto& use : context->leases_)
        {
            use.binding.update_context = context.get();
            use.binding.updated = [](void* value) noexcept { static_cast<ExecutionContext*>(value)->rebind(); };
        }
        auto* result = context.get();
        contexts_.push_back(std::move(context));
        return result;
    }

    RenderResult<void> ExecutionContext::prepareDescriptors() noexcept
    {
        const auto& layout = *code_->descriptor_plan_;
        if (!layout.shared_bindings.empty())
        {
            auto table = owner_->sampledImageArray();
            if (!table) return cxx::unexpected(table.error());
            shared_descriptors_ = std::move(*table);
        }
        const auto draws = std::ranges::count_if(passes_, [](const auto& pass) {
            return pass.recipe->recipe == ERecipe::DRAW;
        });
        if (!layout.private_bindings.empty() && draws)
        {
            const auto sets = std::uint64_t(draws) * 2;
            const auto descriptors = sets * layout.private_bindings.size();
            if (sets > UINT32_MAX || descriptors > UINT32_MAX)
                return cxx::unexpected(RenderError{kCapacity});
            const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, std::uint32_t(descriptors)};
            VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            pool.maxSets = std::uint32_t(sets);
            pool.poolSizeCount = 1;
            pool.pPoolSizes = &size;
            const auto result = LUX_NATIVE("context_descriptor_pool", vkCreateDescriptorPool(device(), &pool,
                nullptr, &descriptor_pool_));
            if (result != VK_SUCCESS) return cxx::unexpected(nativeError(result));
        }
        for (auto& pass : passes_)
        {
            if (pass.recipe->recipe != ERecipe::DRAW) continue;
            pass.descriptors.resize(code_->descriptor_layouts_.size());
            if (shared_descriptors_) pass.descriptors[layout.shared_set] = shared_descriptors_->descriptors();
            if (descriptor_pool_)
            {
                const std::array native_layouts{code_->descriptor_layouts_[layout.private_set],
                    code_->descriptor_layouts_[layout.private_set]};
                VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                allocate.descriptorPool = descriptor_pool_;
                allocate.descriptorSetCount = 2;
                allocate.pSetLayouts = native_layouts.data();
                const auto result = LUX_NATIVE("context_descriptor_set", vkAllocateDescriptorSets(device(), &allocate,
                    pass.private_descriptors.data()));
                if (result != VK_SUCCESS) return cxx::unexpected(nativeError(result));
            }
            // Until the typed parameter plan lands, reject ambiguity instead of silently taking the last float4.
            const PreparedStorageBinding* parameters = nullptr;
            for (const auto& use : layout.programs[pass.recipe->layout_program].uses)
                for (const auto& lease : leases_)
                    if (lease.resource == use.resource && lease.binding.provides_parameters)
                    {
                        if (parameters && parameters != &lease.binding)
                            return cxx::unexpected(RenderError{kStorageContract});
                        parameters = &lease.binding;
                    }
            if (parameters) pass.parameters = &parameters->parameters;
        }
        rebind();
        return {};
    }

    void ExecutionContext::rebind() noexcept
    {
        // Owner safety point. At most one Execute of this Context can be in flight. Never rewrite its bank.
        if (last_use_ && queue_->completed() < last_use_->serial()) descriptor_bank_ = 1u - submitted_bank_;
        const auto& layout = *code_->descriptor_plan_;
        for (const auto& lease : leases_) bindings_[lease.resource].buffer = lease.binding.buffer;
        for (auto& pass : passes_)
        {
            for (std::size_t i = 0; i < pass.buffers.size(); ++i)
                pass.buffers[i].buffer = bindings_[pass.recipe->buffers[i].resource].buffer->native();
            if (pass.recipe->recipe != ERecipe::DRAW || !descriptor_pool_) continue;
            const auto descriptors = pass.private_descriptors[descriptor_bank_];
            pass.descriptors[layout.private_set] = descriptors;
            for (const auto& use : layout.programs[pass.recipe->layout_program].uses)
            {
                if (use.kind != EDescriptorKind::STORAGE_BUFFER) continue;
                const auto* buffer = bindings_[use.resource].buffer;
                const VkDescriptorBufferInfo info{buffer->native(), 0, buffer->size()};
                VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = descriptors;
                write.dstBinding = use.binding;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                write.pBufferInfo = &info;
                vkUpdateDescriptorSets(device(), 1, &write, 0, nullptr);
            }
        }
        for (std::size_t i = 0; i < host_barriers_.size(); ++i)
            host_barriers_[i].buffer = bindings_[code_->hostBarriers()[i].resource].buffer->native();
    }

    RenderResult<void> DeviceSession::release(ExecutionContext* context) noexcept
    {
        const auto found = std::find_if(contexts_.begin(), contexts_.end(), [&](const auto& value) {
            return value.get() == context;
        });
        if (found == contexts_.end()) return cxx::unexpected(RenderError{kWrongOwner});
        if (auto ticket = context->lastUse(); ticket && queue().completed() < ticket->serial())
        {
            SubmissionRequirements requirements;
            requirements.merge(*ticket);
            if (auto result = retirement().retire(context->retirement_, requirements, *found); !result) return result;
        }
        // The retirement record was reserved before Context publication; no release-time capacity allocation.
        context->withdraw();
        contexts_.erase(found);
        return {};
    }

    RenderResult<bool> ExecutionContext::completed() noexcept
    {
        if (!last_use_) return false;
        auto completed = queue_->poll();
        if (!completed) return cxx::unexpected(completed.error());
        return *completed >= last_use_->serial();
    }

    RenderResult<bool> ExecutionContext::execute() noexcept
    {
        if (last_use_)
        {
            if (readers_) return false;
            auto ready = completed();
            if (!ready) return cxx::unexpected(ready.error());
            if (!*ready) return false;
        }
        auto batch = queue_->begin();
        if (!batch)
        {
            if (batch.error().type == kBusy || batch.error().type == kCapacity) return false;
            return cxx::unexpected(batch.error());
        }
        const auto command = batch->native();
        for (const auto& pass : passes_)
        {
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.imageMemoryBarrierCount = static_cast<std::uint32_t>(pass.images.size());
            dependency.pImageMemoryBarriers = pass.images.data();
            dependency.bufferMemoryBarrierCount = static_cast<std::uint32_t>(pass.buffers.size());
            dependency.pBufferMemoryBarriers = pass.buffers.data();
            if (!pass.images.empty() || !pass.buffers.empty())
                LUX_RECORD("cmd_barrier", vkCmdPipelineBarrier2(command, &dependency));
            const auto& recipe = *pass.recipe;
            const auto& destination = bindings_[recipe.destination];
            switch (recipe.recipe)
            {
            case ERecipe::DRAW:
            {
                if (recipe.begin_rendering)
                {
                    VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                    attachment.imageView = destination.view;
                    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    attachment.loadOp = recipe.load;
                    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                    std::copy(recipe.clear_color.begin(), recipe.clear_color.end(), attachment.clearValue.color.float32);
                    const auto extent = destination.image->extent();
                    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
                    rendering.renderArea = {{0, 0}, extent};
                    rendering.layerCount = 1;
                    rendering.colorAttachmentCount = 1;
                    rendering.pColorAttachments = &attachment;
                    LUX_RECORD("cmd_begin", vkCmdBeginRendering(command, &rendering));
                    const VkViewport viewport{0, 0, float(extent.width), float(extent.height), 0, 1};
                    const VkRect2D scissor{{0, 0}, extent};
                    vkCmdSetViewport(command, 0, 1, &viewport);
                    vkCmdSetScissor(command, 0, 1, &scissor);
                }
                if (recipe.bind_pipeline)
                    LUX_RECORD("cmd_bind", vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, recipe.pipeline));
                if (!pass.descriptors.empty())
                    LUX_RECORD("cmd_descriptor", vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        code_->layout(), 0, std::uint32_t(pass.descriptors.size()), pass.descriptors.data(), 0, nullptr));
                vkCmdPushConstants(command, code_->layout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, pass.parameters->data());
                LUX_RECORD("cmd_draw", vkCmdDraw(command, recipe.vertex_count, 1, 0, 0));
                if (recipe.end_rendering) LUX_RECORD("cmd_end", vkCmdEndRendering(command));
                break;
            }
            case ERecipe::CLEAR_IMAGE:
            {
                VkClearColorValue value;
                std::copy(recipe.clear_color.begin(), recipe.clear_color.end(), value.float32);
                const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                vkCmdClearColorImage(command, destination.image->native(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    &value, 1, &range);
                break;
            }
            case ERecipe::FILL_BUFFER:
                vkCmdFillBuffer(command, destination.buffer->native(), 0, VK_WHOLE_SIZE, 0);
                break;
            case ERecipe::COPY_IMAGE_BUFFER:
            {
                const auto& image = *bindings_[recipe.source].image;
                VkBufferImageCopy region{};
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.imageExtent = {image.extent().width, image.extent().height, 1};
                vkCmdCopyImageToBuffer(command, image.native(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    destination.buffer->native(), 1, &region);
                break;
            }
            case ERecipe::COPY_BUFFER:
            {
                const auto* source = bindings_[recipe.source].buffer;
                const VkBufferCopy region{0, 0, source->size()};
                vkCmdCopyBuffer(command, source->native(), destination.buffer->native(), 1, &region);
                break;
            }
            }
        }
        VkDependencyInfo host{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        host.bufferMemoryBarrierCount = static_cast<std::uint32_t>(host_barriers_.size());
        host.pBufferMemoryBarriers = host_barriers_.data();
        if (!host_barriers_.empty()) LUX_RECORD("cmd_barrier", vkCmdPipelineBarrier2(command, &host));
        auto submitted = std::move(*batch).submit();
        if (!submitted) return cxx::unexpected(submitted.error());
        last_use_ = *submitted;
        submitted_bank_ = descriptor_bank_;
        for (const auto& lease : leases_) if (lease.binding.last_use) lease.binding.last_use->merge(*submitted);
        return true;
    }

    RenderResult<std::vector<std::byte>> ExecutionContext::readback(std::uint32_t resource) noexcept
    {
        if (resource == kGraphInvalidIndex) resource = readback_;
        if (resource == kGraphInvalidIndex) return cxx::unexpected(RenderError{kUnsupported});
        const bool invalid = resource >= bindings_.size() || !code_->resources()[resource].readback ||
            !bindings_[resource].buffer;
        if (invalid) return cxx::unexpected(RenderError{kInvalidArgument});
        auto ready = completed();
        if (!ready) return cxx::unexpected(ready.error());
        if (!*ready) return cxx::unexpected(RenderError{kBusy});
        const auto* buffer = bindings_[resource].buffer;
        std::vector<std::byte> result(static_cast<std::size_t>(buffer->size()));
        if (auto read = owner_->read(*buffer, 0, result); !read) return cxx::unexpected(read.error());
        return result;
    }
}
