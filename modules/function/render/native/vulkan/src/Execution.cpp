#include "ExecutionState.hpp"
#include "Native.hpp"
#include <algorithm>

namespace lux::render::vulkan
{
    ExecutionContext::ExecutionContext(SubmissionQueue& queue,
        std::shared_ptr<const CompiledGraph> code, TargetDesc target) noexcept
        : queue_(&queue), code_(std::move(code)), target_(target), bindings_(code_->resources().size()) {}

    ExecutionContext::~ExecutionContext() noexcept
    {
        for (auto& binding : bindings_)
            if (binding.view) LUX_DESTROY("image_view", vkDestroyImageView(device(), binding.view, nullptr));
        for (auto* lease : leases_) --lease->leases;
    }

    RenderResult<ExecutionContext*> DeviceSession::prepare(std::shared_ptr<const CompiledGraph> code,
        const TargetDesc& target, const ScopeIdentity& scope, std::span<const BufferInitializer> initializers) noexcept
    {
        const auto max_extent = device.properties().limits.maxImageDimension2D;
        const bool invalid = !code || code->device() != device.native() || !code->compatible(target) ||
            target.width == 0 || target.height == 0 || target.width > max_extent || target.height > max_extent;
        if (invalid) return cxx::unexpected(RenderError{kInvalidArgument});
        auto context = std::unique_ptr<ExecutionContext>(new ExecutionContext(queue(), std::move(code), target));
        for (std::uint32_t i = 0; i < context->bindings_.size(); ++i)
        {
            auto& binding = context->bindings_[i];
            const auto& plan = context->code_->resources()[i];
            if (!plan.image_usage && !plan.buffer_usage) continue; // G4-pruned resources allocate nothing
            if (const auto* image = std::get_if<GraphImageDesc>(&plan.description))
            {
                const VkExtent2D extent{plan.target_sized ? target.width : image->width,
                    plan.target_sized ? target.height : image->height};
                auto backing = Image::create(allocator, extent, VK_FORMAT_R8G8B8A8_UNORM, plan.image_usage);
                if (!backing) return cxx::unexpected(backing.error());
                binding.image.emplace(std::move(*backing));
                VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                view.image = binding.image->native();
                view.viewType = VK_IMAGE_VIEW_TYPE_2D;
                view.format = VK_FORMAT_R8G8B8A8_UNORM;
                view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                if (plan.image_usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
                {
                    const auto status = LUX_NATIVE("image_view", vkCreateImageView(device.native(), &view,
                        nullptr, &binding.view));
                    if (status != VK_SUCCESS) return cxx::unexpected(nativeError(status));
                }
            }
            else
            {
                const auto size = plan.target_sized ? std::uint64_t(target.width) * target.height * 4 :
                    std::get<GraphBufferDesc>(plan.description).size;
                if (plan.storage)
                {
                    auto identity = scope;
                    const ExecutionContext* attachment = nullptr;
                    switch (plan.storage->scope)
                    {
                    case EResourceScope::GLOBAL: identity = {}; break;
                    case EResourceScope::SCENE: identity.view = {}; break;
                    case EResourceScope::VIEW: break;
                    case EResourceScope::ATTACHMENT: attachment = context.get(); break;
                    }
                    ScopedBuffer* found = nullptr;
                    for (const auto& entry : scoped_)
                    {
                        if (!entry->leases) continue;
                        const bool same_scope = entry->requirement.scope == plan.storage->scope &&
                            entry->scope.scene == identity.scene && entry->scope.view == identity.view &&
                            entry->attachment == attachment;
                        if (!same_scope || entry->key != plan.key) continue;
                        const bool mismatch = entry->requirement != *plan.storage || entry->backing.size() != size ||
                            entry->backing.usage() != plan.buffer_usage;
                        if (mismatch) return cxx::unexpected(RenderError{kInvalidArgument});
                        found = entry.get();
                        break;
                    }
                    if (!found)
                    {
                        auto factory = std::find_if(initializers.begin(), initializers.end(), [&](const auto& value) {
                            return value.type.name() == plan.storage->type.name() && value.abi == plan.storage->abi;
                        });
                        if (factory == initializers.end()) return cxx::unexpected(RenderError{kUnsupported});
                        auto bytes = factory->initialize(size);
                        if (!bytes) return cxx::unexpected(bytes.error());
                        if (bytes->size() != size) return cxx::unexpected(RenderError{kInvalidArgument});
                        auto buffer = Buffer::create(allocator, size, plan.buffer_usage, EMemoryAccess::UPLOAD);
                        if (!buffer) return cxx::unexpected(buffer.error());
                        if (auto written = buffer->write(0, *bytes); !written) return cxx::unexpected(written.error());
                        auto entry = std::make_unique<ScopedBuffer>(ScopedBuffer{
                            plan.key, *plan.storage, identity, attachment, std::move(*buffer)});
                        found = entry.get();
                        scoped_.push_back(std::move(entry));
                        ++scoped_creations_;
                    }
                    ++found->leases;
                    context->leases_.push_back(found);
                    binding.buffer = &found->backing;
                }
                else
                {
                    auto buffer = Buffer::create(allocator, size, plan.buffer_usage,
                        plan.readback ? EMemoryAccess::READBACK : EMemoryAccess::DEVICE);
                    if (!buffer) return cxx::unexpected(buffer.error());
                    binding.owned_buffer.emplace(std::move(*buffer));
                    binding.buffer = &*binding.owned_buffer;
                }
                if (plan.readback && plan.target_sized) context->readback_ = i;
            }
        }
        if (context->readback_ == kGraphInvalidIndex) return cxx::unexpected(RenderError{kUnsupported});
        for (const auto& recipe : context->code_->passes())
        {
            ExecutionContext::Pass pass{&recipe};
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
            vkCmdPipelineBarrier2(command, &dependency);
            const auto& recipe = *pass.recipe;
            const auto& destination = bindings_[recipe.destination];
            switch (recipe.recipe)
            {
            case ERecipe::DRAW:
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
                vkCmdBeginRendering(command, &rendering);
                const VkViewport viewport{0, 0, float(extent.width), float(extent.height), 0, 1};
                const VkRect2D scissor{{0, 0}, extent};
                vkCmdSetViewport(command, 0, 1, &viewport);
                vkCmdSetScissor(command, 0, 1, &scissor);
                vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, recipe.pipeline);
                vkCmdPushConstants(command, code_->layout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, color_.data());
                vkCmdDraw(command, recipe.vertex_count, 1, 0, 0);
                vkCmdEndRendering(command);
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
        vkCmdPipelineBarrier2(command, &host);
        auto submitted = std::move(*batch).submit();
        if (!submitted) return cxx::unexpected(submitted.error());
        last_use_ = *submitted;
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
        if (auto read = buffer->read(0, result); !read) return cxx::unexpected(read.error());
        return result;
    }
}
