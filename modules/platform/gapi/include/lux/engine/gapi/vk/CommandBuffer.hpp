#pragma once
#include <vulkan/vulkan.h>

#include <utility>

namespace lux::gapi::vk
{
    class CommandBuffer
    {
    public:
        CommandBuffer() noexcept = default;

        /// Adopt an already-allocated handle. Allocation belongs to a factory that
        /// can return VkResult and roll back the rest of its construction batch.
        [[nodiscard]] static CommandBuffer adopt(VkCommandBuffer handle) noexcept
        {
            CommandBuffer result;
            result.command_buffer = handle;
            return result;
        }

        CommandBuffer(const CommandBuffer&) = delete;
        CommandBuffer& operator=(const CommandBuffer&) = delete;

        CommandBuffer(CommandBuffer&& other) noexcept
            : command_buffer(std::exchange(other.command_buffer, VkCommandBuffer{}))
        {
        }

        // Rebinding a live command-buffer owner needs the device and command pool
        // that allocated its current handle. Assignment has no valid implementation
        // with this type's state, and no caller needs it.
        CommandBuffer& operator=(CommandBuffer&& other) noexcept = delete;

        void release(VkDevice device, VkCommandPool pool)
        {
            if (command_buffer != VK_NULL_HANDLE)
            {
                vkFreeCommandBuffers(device, pool, 1, &command_buffer);
                command_buffer = VK_NULL_HANDLE;
            }
        }

        void bind(VkPipelineBindPoint bind_point, VkPipeline pipeline)
        {
            vkCmdBindPipeline(command_buffer, bind_point, pipeline);
        }

        void bind(
            VkPipelineBindPoint bind_point,
            VkPipelineLayout layout,
            uint32_t first_set,
            uint32_t descriptor_set_count,
            VkDescriptorSet descriptor_set,
            uint32_t dynamic_offset_count,
            const uint32_t* dynamic_offsets = nullptr
        )
        {
            vkCmdBindDescriptorSets(
                command_buffer,
                VK_PIPELINE_BIND_POINT_GRAPHICS,
                layout,
                first_set,
                descriptor_set_count,
                &descriptor_set,
                dynamic_offset_count,
                dynamic_offsets
            );
        }

        // begin command buffer
        [[nodiscard]] VkResult
        begin(VkCommandBufferUsageFlags flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) noexcept
        {
            VkCommandBufferBeginInfo info{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = flags};
            return vkBeginCommandBuffer(command_buffer, &info);
        }

        // end command buffer
        [[nodiscard]] VkResult end() noexcept
        {
            return vkEndCommandBuffer(command_buffer);
        }

        [[nodiscard]] VkResult reset(VkCommandBufferResetFlags flags = 0) noexcept
        {
            return vkResetCommandBuffer(command_buffer, flags);
        }

        void beginRenderPass(const VkRenderPassBeginInfo& info, VkSubpassContents contents = VK_SUBPASS_CONTENTS_INLINE)
        {
            vkCmdBeginRenderPass(command_buffer, &info, contents);
        }

        void beginRenderPass(
            VkRenderPass render_pass,
            VkFramebuffer framebuffer,
            const VkRect2D& render_area,
            uint32_t clear_value_count,
            const VkClearValue* clear_values,
            VkSubpassContents contents = VK_SUBPASS_CONTENTS_INLINE
        )
        {
            VkRenderPassBeginInfo info{
                .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                .renderPass = render_pass,
                .framebuffer = framebuffer,
                .renderArea = render_area,
                .clearValueCount = clear_value_count,
                .pClearValues = clear_values};
            vkCmdBeginRenderPass(command_buffer, &info, contents);
        }

        void endRenderPass()
        {
            vkCmdEndRenderPass(command_buffer);
        }

        void setViewport(uint32_t first_viewport, uint32_t viewport_count, const VkViewport* viewports)
        {
            vkCmdSetViewport(command_buffer, first_viewport, viewport_count, viewports);
        }

        void setScissor(uint32_t first_scissor, uint32_t scissor_count, const VkRect2D* scissors)
        {
            vkCmdSetScissor(command_buffer, first_scissor, scissor_count, scissors);
        }

        inline operator VkCommandBuffer() const noexcept
        {
            return command_buffer;
        }
        inline const VkCommandBuffer* operator&() const noexcept
        {
            return &command_buffer;
        }

        inline VkCommandBuffer handle() const noexcept
        {
            return command_buffer;
        }
        inline const VkCommandBuffer* handlePtr() const noexcept
        {
            return &command_buffer;
        }

    private:
        VkCommandBuffer command_buffer{VK_NULL_HANDLE};
    };
} // namespace lux::gapi::vk
