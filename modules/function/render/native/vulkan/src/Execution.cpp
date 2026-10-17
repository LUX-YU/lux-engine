#include <lux/engine/render/data/Encoding.hpp>
#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>
#include "Native.hpp"
#include <algorithm>

namespace lux::render::vulkan
{
    ExecutionContext::ExecutionContext(DeviceSession& owner, SubmissionQueue& queue,
        std::shared_ptr<const CompiledGraph> code, TargetDesc target) noexcept
        : owner_(&owner), queue_(&queue), code_(std::move(code)), target_(target), bindings_(code_->resources().size())
    {
        leases_.reserve(bindings_.size());
    }

    ExecutionContext::~ExecutionContext() noexcept
    {
        cancelReadbacks();
        for (auto& binding : bindings_)
        {
            if (binding.view) LUX_DESTROY("image_view", vkDestroyImageView(device(), binding.view, nullptr));
            if (binding.image) owner_->destroyFinished(binding.image);
            if (binding.owned_buffer) owner_->destroyFinished(binding.owned_buffer);
        }
        for (auto& lease : leases_)
        {
            lease.binding.reference.reset();
            owner_->resources().release(lease.id);
        }
        if (owner_->resources().valid(domain_)) (void)owner_->resources().closeDomain(domain_);
        if (!published_)
            for (const auto& lease : leases_)
                if (lease.created && owner_->resources().native(lease.id)) owner_->resources().rollback(lease.id);
    }

    RenderResult<ExecutionContext*> DeviceSession::prepare(std::shared_ptr<const CompiledGraph> code,
        const TargetDesc& target, const ResourceDomain& view, std::span<const StorageArgument> arguments) noexcept
    {
        const auto max_extent = device_.properties().limits.maxImageDimension2D;
        const bool invalid = !code || code->device() != device_.native() || !code->compatible(target) ||
            target.width == 0 || target.height == 0 || target.width > max_extent || target.height > max_extent;
        if (invalid) return cxx::unexpected(RenderError{kInvalidArgument});
        if (!view.id().isNull() && !resources_->valid(view)) return cxx::unexpected(RenderError{kStorageDomain});
        auto context = std::unique_ptr<ExecutionContext>(new ExecutionContext(*this, queue(), std::move(code), target));
        if (resources_->valid(view))
        {
            auto domain = resources_->createDomain(view, EResourceScope::ATTACHMENT);
            if (!domain) return cxx::unexpected(domain.error());
            context->domain_ = std::move(*domain);
        }
        std::vector<PreparedStorageBinding*> prepared_bindings(context->bindings_.size());
        for (std::uint32_t i = 0; i < context->bindings_.size(); ++i)
        {
            auto& binding = context->bindings_[i];
            const auto& plan = context->code_->resources()[i];
            if (!plan.image_usage && !plan.buffer_usage) continue; // G4-pruned resources allocate nothing
            if (plan.storage)
            {
                const auto* factory = resources_->factory(plan.storage->type.view());
                // Compiler already proved the frozen Factory/ABI/binding contract.
                auto domain = plan.storage->scope == EResourceScope::GLOBAL ?
                    RenderResult<ResourceDomain>{resources_->root()} :
                    resources_->ancestor(context->domain_, plan.storage->scope);
                if (!domain) return cxx::unexpected(domain.error());
                const auto key = plan.storage->key.empty() ? plan.key : plan.storage->key;
                const auto argument = std::ranges::find(arguments, plan.key, &StorageArgument::resource_key);
                const auto bytes = argument == arguments.end() ? std::span<const std::byte>{} : argument->bytes.view();
                if (factory->description->binding_schema)
                {
                    const auto& schema = *factory->description->binding_schema;
                    if (auto valid = validateBoundEncodedData({schema, bytes, {}}, {schema.fixed_size, 0}); !valid)
                        return cxx::unexpected(valid.error());
                }
                else if (!bytes.empty()) return cxx::unexpected(RenderError{kStorageContract});
                const auto previous = resources_->find(*domain, key, *factory->description);
                auto instance = resources_->getOrCreate(*domain, key, *factory->description, nullptr);
                if (!instance) return cxx::unexpected(instance.error());
                resources_->retain(*instance);
                auto& lease = context->leases_.emplace_back(*instance, PreparedStorageBinding{}, !previous);
                const auto& ops = *static_cast<const StorageBindingOps*>(factory->binding_ops);
                if (auto prepared = ops.prepare(resources_->native(*instance), plan, bytes, lease.binding); !prepared)
                    return cxx::unexpected(prepared.error());
                binding.buffer = lease.binding.buffer;
                prepared_bindings[i] = &lease.binding;
                continue;
            }
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
        if (context->readback_ == kGraphInvalidIndex) return cxx::unexpected(RenderError{kUnsupported});
        for (const auto& recipe : context->code_->passes())
        {
            ExecutionContext::Pass pass{&recipe};
            pass.parameters = &context->color_;
            for (auto index : recipe.descriptor_resources)
            {
                auto& binding = *prepared_bindings[index];
                pass.descriptors.push_back(binding.descriptors);
                if (binding.provides_parameters) pass.parameters = &binding.parameters;
            }
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
        context->published_ = true;
        auto* result = context.get();
        contexts_.push_back(std::move(context));
        return result;
    }

    RenderResult<void> DeviceSession::release(ExecutionContext* context) noexcept
    {
        const auto found = std::find_if(contexts_.begin(), contexts_.end(), [&](const auto& value) {
            return value.get() == context;
        });
        if (found == contexts_.end()) return cxx::unexpected(RenderError{kWrongOwner});
        if (auto ticket = context->lastUse(); ticket && queue().completed() < ticket->serial())
        {
            if (auto result = retirement().retire(*ticket, std::move(*found)); !result) return result;
        }
        // Retirement admission must succeed before detaching the logical readback uses.
        context->cancelReadbacks();
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
        for (const auto& lease : leases_) if (lease.binding.last_use) lease.binding.last_use->merge(*submitted);
        return true;
    }

    RenderResult<std::vector<std::byte>> ExecutionContext::readback(std::uint32_t resource) noexcept
    {
        auto ready = completed();
        if (!ready) return cxx::unexpected(ready.error());
        if (!*ready) return cxx::unexpected(RenderError{kBusy});
        if (resource == kGraphInvalidIndex) resource = readback_;
        const bool invalid = resource >= bindings_.size() || !code_->resources()[resource].readback ||
            !bindings_[resource].buffer;
        if (invalid) return cxx::unexpected(RenderError{kInvalidArgument});
        const auto* buffer = bindings_[resource].buffer;
        std::vector<std::byte> result(static_cast<std::size_t>(buffer->size()));
        if (auto read = owner_->read(*buffer, 0, result); !read) return cxx::unexpected(read.error());
        return result;
    }
}
