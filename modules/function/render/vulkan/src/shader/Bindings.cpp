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
            const bool wrong_dimension = field.dimension != "2D" || image.samples() != VK_SAMPLE_COUNT_1_BIT;
            const bool wrong_storage = field.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE &&
                                       (storageFormat(field.image_format) == VK_FORMAT_UNDEFINED ||
                                        storageFormat(field.image_format) != image.format());
            return !wrong_dimension && !wrong_storage;
        }
    } // namespace

    RenderResult<BoundDescriptorSets> BoundDescriptorSets::create(
        const VulkanDevice& device,
        const NativeShaderProgram& program,
        std::span<const OwnerDescriptorValue> values
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
                                const bool wrong_backing = nativeTextureFormat(texture.format) != view.format() ||
                                                           texture.width != view.extent().width ||
                                                           texture.height != view.extent().height ||
                                                           texture.depth != 1 || texture.mip_count != 1 ||
                                                           texture.array_layers != 1 || texture.samples != 1;
                                const bool wrong_range = !range || range->base_mip != 0 || range->mip_count != 1 ||
                                                         range->base_layer != 0 || range->layer_count != 1 ||
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
                        if (written && is_dynamic)
                        {
                            const auto alignment = field.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
                                                       ? limits.minUniformBufferOffsetAlignment
                                                       : limits.minStorageBufferOffsetAlignment;
                            dynamic.push_back({alignment, backing.size() - buffer->offset - buffer->range});
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
                    }
                    else if (field.type == VK_DESCRIPTOR_TYPE_SAMPLER)
                    {
                        written = pool->writeSampler(
                            *set,
                            binding.binding,
                            element,
                            std::get<std::reference_wrapper<const Sampler>>(value.value).get()
                        );
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
            scalar_size
        };
    }

    BoundDescriptorSets::BoundDescriptorSets(
        DescriptorPool pool,
        std::vector<VkDescriptorSet> sets,
        VkPipelineLayout layout,
        VkPipelineBindPoint point,
        std::vector<DynamicRange> dynamic,
        std::vector<VkPushConstantRange> push,
        std::size_t scalar_size
    ) noexcept
        : pool_(std::move(pool)), sets_(std::move(sets)), layout_(layout), point_(point), dynamic_(std::move(dynamic)),
          push_(std::move(push)), scalar_size_(scalar_size)
    {
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
