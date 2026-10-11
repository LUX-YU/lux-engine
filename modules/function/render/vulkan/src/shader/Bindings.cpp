#include <algorithm>
#include <lux/engine/render/vulkan/shader/Bindings.hpp>
#include <lux/engine/render/vulkan/shader/Format.hpp>

namespace lux::render::vulkan
{
    namespace
    {
        VkFormat storageFormat(std::string_view name) noexcept
        {
            if (name == "rgba32f")
            {
                return VK_FORMAT_R32G32B32A32_SFLOAT;
            }
            if (name == "rgba16f")
            {
                return VK_FORMAT_R16G16B16A16_SFLOAT;
            }
            if (name == "r32f")
            {
                return VK_FORMAT_R32_SFLOAT;
            }
            if (name == "rgba8")
            {
                return VK_FORMAT_R8G8B8A8_UNORM;
            }
            return VK_FORMAT_UNDEFINED;
        }

        bool matchesImage(const OwnerField& field, const ImageView& image) noexcept
        {
            const bool is_array = image.type() == VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            const bool is_ms = image.samples() != VK_SAMPLE_COUNT_1_BIT;
            const std::string_view dimension =
                is_ms ? (is_array ? "2DMSArray" : "2DMS") : (is_array ? "2DArray" : "2D");
            const bool wrong_dimension = field.dimension != dimension;
            const bool wrong_storage = field.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE &&
                                       (storageFormat(field.image_format) == VK_FORMAT_UNDEFINED ||
                                        storageFormat(field.image_format) != image.format());
            return !wrong_dimension && !wrong_storage;
        }
    } // namespace

    RenderResult<BoundDescriptorSets> BoundDescriptorSets::create(
        const VulkanDevice& device,
        const NativeShaderProgram& program,
        std::span<const OwnerDescriptorValue> values,
        EDescriptorUpdates updates
    ) noexcept
    {
        if (program.device() != device.native())
        {
            return cxx::unexpected(RenderError{kWrongOwner});
        }
        const auto& layout = program.identity().layout;
        std::vector<VkDescriptorPoolSize> sizes;
        std::size_t expected_values = 0;
        for (const auto& owner : layout.owners)
        {
            for (const auto& field : owner.fields)
            {
                expected_values += field.count;
                auto type =
                    std::find_if(sizes.begin(), sizes.end(), [&](const auto& item) { return item.type == field.type; });
                if (type == sizes.end())
                {
                    sizes.push_back({field.type, field.count});
                }
                else
                {
                    type->descriptorCount += field.count;
                }
            }
        }
        if (values.size() != expected_values)
        {
            return cxx::unexpected(RenderError{kInvalidArgument, {1}});
        }
        // An empty descriptor interface still owns a small valid native pool, with no allocated sets.
        if (sizes.empty())
        {
            sizes.push_back({VK_DESCRIPTOR_TYPE_SAMPLER, 1});
        }
        auto pool = DescriptorPool::create(
            device,
            static_cast<std::uint32_t>(std::max<std::size_t>(1, layout.sets.size())),
            sizes
        );
        if (!pool)
        {
            return cxx::unexpected(pool.error());
        }
        std::vector<VkDescriptorSet> sets;
        std::vector<DynamicRange> dynamic;
        std::vector<WriteRecipe> writes;
        const bool rewritable = updates == EDescriptorUpdates::COMPLETED_ONLY;
        if (rewritable)
        {
            writes.reserve(values.size());
        }
        const auto& limits = device.properties().limits;
        for (std::size_t set_index = 0; set_index < layout.sets.size(); ++set_index)
        {
            auto set = pool->allocate(program.setLayouts()[set_index]);
            if (!set)
            {
                return cxx::unexpected(set.error());
            }
            sets.push_back(*set);
            for (const auto& binding : layout.sets[set_index].bindings)
            {
                const auto owner = std::find_if(
                    layout.owners.begin(),
                    layout.owners.end(),
                    [&](const auto& candidate) { return candidate.identity == binding.owner; }
                );
                const auto& field = owner->fields[binding.owner_field];
                for (std::uint32_t element = 0; element < field.count; ++element)
                {
                    const auto matches = [&](const auto& value)
                    {
                        return value.owner == owner->identity && value.semantic == field.semantic &&
                               value.element == element;
                    };
                    if (std::count_if(values.begin(), values.end(), matches) != 1)
                    {
                        return cxx::unexpected(RenderError{kInvalidArgument, {2, set_index, binding.binding}});
                    }
                    const auto& value = *std::find_if(values.begin(), values.end(), matches);
                    // This pass may use only a subset of the complete owner shape. Validate each selected
                    // native view against that pass's logical resource facts before making it executable.
                    const auto& identity = program.identity();
                    const auto& pass = identity.graph.passes[identity.pass.value() - 1];
                    for (const auto& location : layout.fields)
                    {
                        if (location.owner != owner->identity || location.set != set_index ||
                            location.binding != binding.binding)
                        {
                            continue;
                        }
                        for (const auto& logical_field : pass.bindings)
                        {
                            const auto semantic =
                                logical_field.semantic.empty() ? logical_field.path : logical_field.semantic;
                            if (semantic != field.semantic || logical_field.array_element != element ||
                                logical_field.owner != owner->category)
                            {
                                continue;
                            }
                            const auto* resource = std::get_if<ShaderResourceBinding>(&logical_field.value);
                            if (!resource)
                            {
                                continue;
                            }
                            const auto& declaration = identity.graph.resources[resource->resource.value() - 1];
                            if (const auto* image = std::get_if<ImageDescriptorValue>(&value.value))
                            {
                                if (declaration.kind() != EGraphResourceKind::IMAGE)
                                {
                                    return cxx::unexpected(RenderError{kInvalidArgument});
                                }
                                const auto& texture = declaration.texture();
                                const auto& view = image->image.get();
                                const auto* range = std::get_if<ImageRange>(&resource->range);
                                const auto& backing = view.backingDescription();
                                const auto& native_range = view.range();
                                const bool wrong_backing =
                                    nativeTextureFormat(texture.format) != view.format() ||
                                    texture.width != backing.extent.width || texture.height != backing.extent.height ||
                                    texture.depth != 1 || texture.mip_count != backing.mip_levels ||
                                    texture.array_layers != backing.array_layers || texture.samples != backing.samples;
                                const bool wrong_range = !range || range->base_mip != native_range.baseMipLevel ||
                                                         range->mip_count != native_range.levelCount ||
                                                         range->base_layer != native_range.baseArrayLayer ||
                                                         range->layer_count != native_range.layerCount ||
                                                         static_cast<std::uint32_t>(range->aspect) != view.aspect();
                                if (wrong_backing || wrong_range)
                                {
                                    return cxx::unexpected(RenderError{kInvalidArgument, {5}});
                                }
                            }
                            else if (const auto* buffer = std::get_if<BufferDescriptorValue>(&value.value))
                            {
                                const auto* range = std::get_if<BufferRange>(&resource->range);
                                if (!range || buffer->range != range->byte_count)
                                {
                                    return cxx::unexpected(RenderError{kInvalidArgument, {6}});
                                }
                            }
                        }
                    }
                    RenderResult<void> written = cxx::unexpected(RenderError{kInvalidArgument});
                    const auto input = static_cast<std::uint32_t>(&value - values.data());
                    if (const auto* buffer = std::get_if<BufferDescriptorValue>(&value.value))
                    {
                        const auto& backing = buffer->buffer.get();
                        const bool invalid_block =
                            buffer->range < field.element_stride ||
                            (field.element_stride != 0 && buffer->range % field.element_stride != 0);
                        if (invalid_block)
                        {
                            return cxx::unexpected(RenderError{kInvalidArgument, {3}});
                        }
                        written = pool->writeBuffer(
                            *set,
                            binding.binding,
                            element,
                            field.type,
                            backing,
                            buffer->offset,
                            buffer->range
                        );
                        const bool is_dynamic = field.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
                                                field.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
                        const bool uniform = field.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
                                             field.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
                        const auto alignment =
                            uniform ? limits.minUniformBufferOffsetAlignment : limits.minStorageBufferOffsetAlignment;
                        BufferWrite shape{
                            buffer->offset,
                            buffer->range,
                            alignment,
                            static_cast<VkBufferUsageFlags>(
                                uniform ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                            ),
                            {}
                        };
                        if (written && is_dynamic)
                        {
                            shape.dynamic = static_cast<std::uint32_t>(dynamic.size());
                            dynamic.push_back({alignment, backing.size() - buffer->offset - buffer->range});
                        }
                        if (rewritable)
                        {
                            writes.push_back({*set, binding.binding, element, input, field.type, shape});
                        }
                    }
                    else if (const auto* image = std::get_if<ImageDescriptorValue>(&value.value))
                    {
                        if (!matchesImage(field, image->image.get()))
                        {
                            return cxx::unexpected(RenderError{kInvalidArgument, {7}});
                        }
                        written = pool->writeImage(
                            *set,
                            binding.binding,
                            element,
                            field.type,
                            image->image.get(),
                            image->layout,
                            image->combined_sampler
                        );
                        const auto& view = image->image.get();
                        const VkImageUsageFlags usage =
                            field.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE      ? VK_IMAGE_USAGE_STORAGE_BIT
                            : field.type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT ? VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT
                                                                                : VK_IMAGE_USAGE_SAMPLED_BIT;
                        if (rewritable)
                        {
                            writes.push_back(
                                {*set,
                                 binding.binding,
                                 element,
                                 input,
                                 field.type,
                                 ImageWrite{
                                     view.backingDescription(),
                                     view.range(),
                                     view.type(),
                                     image->layout,
                                     usage,
                                     image->combined_sampler != nullptr
                                 }}
                            );
                        }
                    }
                    else if (field.type == VK_DESCRIPTOR_TYPE_SAMPLER)
                    {
                        written = pool->writeSampler(
                            *set,
                            binding.binding,
                            element,
                            std::get<std::reference_wrapper<const Sampler>>(value.value).get()
                        );
                        if (rewritable)
                        {
                            writes.push_back({*set, binding.binding, element, input, field.type, SamplerWrite{}});
                        }
                    }
                    if (!written)
                    {
                        return cxx::unexpected(written.error());
                    }
                }
            }
        }
        if (dynamic.size() != layout.dynamic_count)
        {
            return cxx::unexpected(RenderError{kInvalidArgument, {4}});
        }
        std::vector<VkPushConstantRange> push;
        for (const auto& range : layout.push_ranges)
        {
            push.push_back({nativeStages(range.stages), range.offset, range.size});
        }
        const auto point = std::holds_alternative<ComputeDescription>(program.identity().pipeline)
                               ? VK_PIPELINE_BIND_POINT_COMPUTE
                               : VK_PIPELINE_BIND_POINT_GRAPHICS;
        const auto scalar_size = program.identity().graph.passes[program.identity().pass.value() - 1].scalar_size;
        return BoundDescriptorSets{
            std::move(*pool),
            std::move(sets),
            program.pipelineLayout(),
            point,
            std::move(dynamic),
            std::move(push),
            scalar_size,
            device.native(),
            std::move(writes),
            updates
        };
    }

    BoundDescriptorSets::BoundDescriptorSets(
        DescriptorPool pool,
        std::vector<VkDescriptorSet> sets,
        VkPipelineLayout layout,
        VkPipelineBindPoint point,
        std::vector<DynamicRange> dynamic,
        std::vector<VkPushConstantRange> push,
        std::size_t scalar_size,
        VkDevice device,
        std::vector<WriteRecipe> writes,
        EDescriptorUpdates updates
    ) noexcept
        : sets_(std::move(sets)), layout_(layout), point_(point), updates_(updates), dynamic_(std::move(dynamic)),
          push_(std::move(push)), scalar_size_(scalar_size), pool_(std::move(pool)), device_(device),
          writes_(std::move(writes))
    {
    }

    RenderResult<void> BoundDescriptorSets::rewrite(
        std::span<const VDescriptorValue> values,
        std::span<const SubmissionTicket> last_uses
    ) noexcept
    {
        if (updates_ != EDescriptorUpdates::COMPLETED_ONLY || values.size() != writes_.size())
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        for (const auto& ticket : last_uses)
        {
            if (ticket.owner().device() != device_ || !ticket.owner().owns(ticket))
            {
                return cxx::unexpected(RenderError{kWrongOwner});
            }
            if (ticket.owner().deviceLost())
            {
                return cxx::unexpected(nativeError(VK_ERROR_DEVICE_LOST));
            }
            if (ticket.serial() > ticket.owner().completed())
            {
                return cxx::unexpected(RenderError{kBusy});
            }
        }
        // Validate the entire candidate before the first vkUpdateDescriptorSets.
        for (const auto& recipe : writes_)
        {
            const auto& value = values[recipe.input];
            bool valid = false;
            if (const auto* shape = std::get_if<BufferWrite>(&recipe.shape))
            {
                const auto* buffer = std::get_if<BufferDescriptorValue>(&value);
                valid = buffer && buffer->buffer.get().device() == device_ && buffer->buffer.get().native() &&
                        buffer->offset % shape->alignment == 0 && buffer->range == shape->range &&
                        buffer->offset <= buffer->buffer.get().size() &&
                        buffer->range <= buffer->buffer.get().size() - buffer->offset &&
                        (buffer->buffer.get().usage() & shape->usage) == shape->usage;
            }
            else if (const auto* shape = std::get_if<ImageWrite>(&recipe.shape))
            {
                const auto* image = std::get_if<ImageDescriptorValue>(&value);
                if (image)
                {
                    const auto& view = image->image.get();
                    const auto& actual = view.backingDescription();
                    const auto& range = view.range();
                    const auto& expected = shape->backing;
                    valid = view.device() == device_ && view.native() && view.type() == shape->type &&
                            image->layout == shape->layout && actual.extent.width == expected.extent.width &&
                            actual.extent.height == expected.extent.height && actual.format == expected.format &&
                            actual.samples == expected.samples && actual.mip_levels == expected.mip_levels &&
                            actual.array_layers == expected.array_layers &&
                            (actual.usage & shape->usage) == shape->usage &&
                            range.aspectMask == shape->range.aspectMask &&
                            range.baseMipLevel == shape->range.baseMipLevel &&
                            range.levelCount == shape->range.levelCount &&
                            range.baseArrayLayer == shape->range.baseArrayLayer &&
                            range.layerCount == shape->range.layerCount &&
                            (image->combined_sampler != nullptr) == shape->combined &&
                            (!image->combined_sampler ||
                             (image->combined_sampler->device() == device_ && image->combined_sampler->native()));
                }
            }
            else if (const auto* sampler = std::get_if<std::reference_wrapper<const Sampler>>(&value))
            {
                valid = sampler->get().device() == device_ && sampler->get().native();
            }
            if (!valid)
            {
                return cxx::unexpected(RenderError{kInvalidArgument, {recipe.input}});
            }
        }
        for (const auto& recipe : writes_)
        {
            const auto& value = values[recipe.input];
            RenderResult<void> written;
            if (const auto* buffer = std::get_if<BufferDescriptorValue>(&value))
            {
                written = pool_.writeBuffer(
                    recipe.set,
                    recipe.binding,
                    recipe.element,
                    recipe.type,
                    buffer->buffer,
                    buffer->offset,
                    buffer->range
                );
                const auto& shape = std::get<BufferWrite>(recipe.shape);
                if (shape.dynamic)
                {
                    dynamic_[*shape.dynamic].maximum_offset =
                        buffer->buffer.get().size() - buffer->offset - buffer->range;
                }
            }
            else if (const auto* image = std::get_if<ImageDescriptorValue>(&value))
            {
                written = pool_.writeImage(
                    recipe.set,
                    recipe.binding,
                    recipe.element,
                    recipe.type,
                    image->image,
                    image->layout,
                    image->combined_sampler
                );
            }
            else
            {
                written = pool_.writeSampler(
                    recipe.set,
                    recipe.binding,
                    recipe.element,
                    std::get<std::reference_wrapper<const Sampler>>(value)
                );
            }
            if (!written)
            {
                std::terminate(); // Identical constraints were validated before any native write.
            }
        }
        return {};
    }

    RenderResult<void> BoundDescriptorSets::bind(
        VkCommandBuffer command,
        std::span<const std::uint32_t> dynamic_offsets,
        std::span<const std::byte> invocation_scalars
    ) const noexcept
    {
        const bool invalid_input =
            !command || dynamic_offsets.size() != dynamic_.size() || invocation_scalars.size() != scalar_size_;
        if (invalid_input)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        for (std::size_t i = 0; i < dynamic_.size(); ++i)
        {
            const bool invalid_offset =
                dynamic_offsets[i] % dynamic_[i].alignment != 0 || dynamic_offsets[i] > dynamic_[i].maximum_offset;
            if (invalid_offset)
            {
                return cxx::unexpected(RenderError{kInvalidArgument, {i}});
            }
        }
        if (!sets_.empty())
        {
            vkCmdBindDescriptorSets(
                command,
                point_,
                layout_,
                0,
                static_cast<std::uint32_t>(sets_.size()),
                sets_.data(),
                static_cast<std::uint32_t>(dynamic_offsets.size()),
                dynamic_offsets.data()
            );
        }
        for (const auto& range : push_)
        {
            vkCmdPushConstants(
                command,
                layout_,
                range.stageFlags,
                range.offset,
                range.size,
                invocation_scalars.data() + range.offset
            );
        }
        return {};
    }

    VkClearColorValue nativeColorClear(const ColorClearValue& clear) noexcept
    {
        VkClearColorValue result{};
        if (const auto* value = clear.getIf<FloatColorClear>())
        {
            std::copy(value->values.begin(), value->values.end(), result.float32);
        }
        else if (const auto* value = clear.getIf<SintColorClear>())
        {
            std::copy(value->values.begin(), value->values.end(), result.int32);
        }
        else
        {
            const auto& unsigned_value = *clear.getIf<UintColorClear>();
            std::copy(unsigned_value.values.begin(), unsigned_value.values.end(), result.uint32);
        }
        return result;
    }
} // namespace lux::render::vulkan
