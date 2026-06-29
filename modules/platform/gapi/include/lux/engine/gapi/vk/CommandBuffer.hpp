#pragma once
#include "lux/engine/gapi/vk/Object.hpp"
#include <vulkan/vulkan.h>

namespace lux::gapi::vk
{
	class CommandBuffer 
	{
	public:
		CommandBuffer() : command_buffer(VK_NULL_HANDLE) {}

		CommandBuffer(VkDevice device,  VkCommandPool pool, VkCommandBufferLevel level = VK_COMMAND_BUFFER_LEVEL_PRIMARY)
		{
			VkCommandBufferAllocateInfo info{
				.sType				= VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
				.pNext				= nullptr,
				.commandPool		= pool,
				.level				= level,
				.commandBufferCount = 1
			};

			VK_FUNC_INVOKE(vkAllocateCommandBuffers, "Failed to allocate CommandBuffer object", device, &info, &command_buffer);
		}

		CommandBuffer(const CommandBuffer&) = delete;
		CommandBuffer& operator=(const CommandBuffer&) = delete;

		CommandBuffer(CommandBuffer&& other) noexcept
		{
			command_buffer = other.command_buffer;
			other.command_buffer = VK_NULL_HANDLE;
		}

		CommandBuffer& operator=(CommandBuffer&& other) noexcept
		{
			command_buffer = other.command_buffer;
			other.command_buffer = VK_NULL_HANDLE;
			return *this;
		}

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

		void bind(VkPipelineBindPoint bind_point, VkPipelineLayout layout, uint32_t first_set, uint32_t descriptor_set_count, VkDescriptorSet descriptor_set, uint32_t dynamic_offset_count, const uint32_t* dynamic_offsets = nullptr)
		{
			vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, first_set, descriptor_set_count, &descriptor_set, dynamic_offset_count, dynamic_offsets);
		}

		// begin command buffer
		void begin(VkCommandBufferUsageFlags flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT)
		{
			VkCommandBufferBeginInfo info{
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
				.flags = flags
			};
			vkBeginCommandBuffer(command_buffer, &info);
		}

		// end command buffer
		void end()
		{
			vkEndCommandBuffer(command_buffer);
		}

		VkResult reset(VkCommandBufferResetFlags flags = 0)
		{
			return vkResetCommandBuffer(command_buffer, flags);
		}

		void beginRenderPass(const VkRenderPassBeginInfo& info, VkSubpassContents contents = VK_SUBPASS_CONTENTS_INLINE)
		{
			vkCmdBeginRenderPass(command_buffer, &info, contents);
		}

		void beginRenderPass(VkRenderPass render_pass, VkFramebuffer framebuffer, const VkRect2D& render_area, uint32_t clear_value_count, const VkClearValue* clear_values, VkSubpassContents contents = VK_SUBPASS_CONTENTS_INLINE)
		{
			VkRenderPassBeginInfo info{
				.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
				.renderPass		 = render_pass,
				.framebuffer	 = framebuffer,
				.renderArea		 = render_area,
				.clearValueCount = clear_value_count,
				.pClearValues	 = clear_values
			};
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

		inline operator VkCommandBuffer() const noexcept { return command_buffer; }
		inline const VkCommandBuffer* operator&() const noexcept { return &command_buffer; }

		inline VkCommandBuffer handle() const noexcept { return command_buffer; }
		inline const VkCommandBuffer* handlePtr() const noexcept { return &command_buffer; }

	private:
		VkCommandBuffer command_buffer;
	};
} // namespace lux::gapi::vk