#pragma once

#include <functional>
#include <lux/engine/render/vulkan/descriptor/ImageBindings.hpp>
#include <lux/engine/render/vulkan/shader/Program.hpp>

namespace lux::render::vulkan
{
    struct BufferDescriptorValue
    {
        std::reference_wrapper<const Buffer> buffer;
        VkDeviceSize offset, range;
    };

    struct ImageDescriptorValue
    {
        std::reference_wrapper<const ImageView> image;
        VkImageLayout layout;
        const Sampler* combined_sampler{};
    };

    using VDescriptorValue =
        std::variant<BufferDescriptorValue, ImageDescriptorValue, std::reference_wrapper<const Sampler>>;

    struct OwnerDescriptorValue
    {
        ShaderOwnerId owner;
        std::string_view semantic;
        std::uint32_t element;
        VDescriptorValue value;
    };

    // Complete immutable sets. Creation writes each declared descriptor once, before publication.
    // Borrows program layout and all native resources through the last GPU use. No update-in-flight API.
    class BoundDescriptorSets
    {
    public:
        [[nodiscard]] static RenderResult<BoundDescriptorSets> create(
            const VulkanDevice& device,
            const NativeShaderProgram& program,
            std::span<const OwnerDescriptorValue> values
        ) noexcept;

        static RenderResult<BoundDescriptorSets>
        create(const VulkanDevice&, const NativeShaderProgram&&, std::span<const OwnerDescriptorValue>) = delete;

        BoundDescriptorSets(BoundDescriptorSets&&) noexcept = default;
        BoundDescriptorSets& operator=(BoundDescriptorSets&&) = delete;
        BoundDescriptorSets(const BoundDescriptorSets&) = delete;
        BoundDescriptorSets& operator=(const BoundDescriptorSets&) = delete;

        // Fixed recipes only. No name lookup, reflection, allocation or lazy native creation.
        [[nodiscard]] RenderResult<void> bind(
            VkCommandBuffer command,
            std::span<const std::uint32_t> dynamic_offsets,
            std::span<const std::byte> invocation_scalars
        ) const noexcept;

    private:
        struct DynamicRange
        {
            VkDeviceSize alignment, maximum_offset;
        };

        BoundDescriptorSets(
            DescriptorPool pool,
            std::vector<VkDescriptorSet> sets,
            VkPipelineLayout layout,
            VkPipelineBindPoint point,
            std::vector<DynamicRange> dynamic,
            std::vector<VkPushConstantRange> push,
            std::size_t scalar_size
        ) noexcept;

        DescriptorPool pool_;
        std::vector<VkDescriptorSet> sets_;
        VkPipelineLayout layout_;
        VkPipelineBindPoint point_;
        std::vector<DynamicRange> dynamic_;
        std::vector<VkPushConstantRange> push_;
        std::size_t scalar_size_;
    };

    [[nodiscard]] VkClearColorValue nativeColorClear(const ColorClearValue& clear) noexcept;
} // namespace lux::render::vulkan
