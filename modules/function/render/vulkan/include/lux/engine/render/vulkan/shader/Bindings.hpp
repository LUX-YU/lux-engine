#pragma once

#include <functional>
#include <lux/engine/render/vulkan/descriptor/ImageBindings.hpp>
#include <lux/engine/render/vulkan/shader/Program.hpp>
#include <lux/engine/render/vulkan/transfer/Submission.hpp>

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

    enum class EDescriptorUpdates
    {
        IMMUTABLE,
        COMPLETED_ONLY
    };

    // Complete sets, with fixed layout. Borrows native resources through the last GPU use.
    // A rewrite requires completion of ALL prior users; it never updates in-flight descriptors.
    class BoundDescriptorSets
    {
    public:
        [[nodiscard]] static RenderResult<BoundDescriptorSets> create(
            const VulkanDevice& device,
            const NativeShaderProgram& program,
            std::span<const OwnerDescriptorValue> values,
            EDescriptorUpdates updates = EDescriptorUpdates::IMMUTABLE
        ) noexcept;

        static RenderResult<BoundDescriptorSets>
        create(
            const VulkanDevice&,
            const NativeShaderProgram&&,
            std::span<const OwnerDescriptorValue>,
            EDescriptorUpdates = EDescriptorUpdates::IMMUTABLE
        ) = delete;

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

        // Values retain the order supplied to create(). Fixed numeric recipes only.
        // last_uses is an explicit borrow proof supplied by the execution owner, not a wait request.
        [[nodiscard]] RenderResult<void> rewrite(
            std::span<const VDescriptorValue> values,
            std::span<const SubmissionTicket> last_uses
        ) noexcept;

    private:
        struct DynamicRange
        {
            VkDeviceSize alignment, maximum_offset;
        };

        struct BufferWrite
        {
            VkDeviceSize offset, range, alignment;
            VkBufferUsageFlags usage;
            std::optional<std::uint32_t> dynamic;
        };

        struct ImageWrite
        {
            ImageDescription backing;
            VkImageSubresourceRange range;
            VkImageViewType type;
            VkImageLayout layout;
            VkImageUsageFlags usage;
            bool combined;
        };

        struct SamplerWrite
        {
        };

        using VWriteShape = std::variant<BufferWrite, ImageWrite, SamplerWrite>;

        struct WriteRecipe
        {
            VkDescriptorSet set;
            std::uint32_t binding, element, input;
            VkDescriptorType type;
            VWriteShape shape;
        };

        BoundDescriptorSets(
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
        ) noexcept;

        // Keep the direct bind recipe together; cold ownership and rewrite data follow it.
        std::vector<VkDescriptorSet> sets_;
        VkPipelineLayout layout_;
        VkPipelineBindPoint point_;
        EDescriptorUpdates updates_;
        std::vector<DynamicRange> dynamic_;
        std::vector<VkPushConstantRange> push_;
        std::size_t scalar_size_;
        DescriptorPool pool_;
        VkDevice device_;
        std::vector<WriteRecipe> writes_;
    };

    [[nodiscard]] VkClearColorValue nativeColorClear(const ColorClearValue& clear) noexcept;
} // namespace lux::render::vulkan
