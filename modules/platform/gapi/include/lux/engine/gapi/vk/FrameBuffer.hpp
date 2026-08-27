#pragma once
#include "lux/engine/gapi/vk/Object.hpp"
#include <vulkan/vulkan.h>

namespace lux::gapi::vk
{
    class FrameBufferBuilder;
    class FrameBuffer
    {
    public:
        using Builder = FrameBufferBuilder;

        FrameBuffer() : frame_buffer(VK_NULL_HANDLE)
        {
        }

        FrameBuffer(VkDevice device, const VkFramebufferCreateInfo& info, VkAllocationCallbacks* allocator = nullptr)
        {
            VK_FUNC_INVOKE(
                vkCreateFramebuffer,
                "Failed to create FrameBuffer object",
                device,
                &info,
                allocator,
                &frame_buffer
            );
        }

        FrameBuffer(const FrameBuffer&) = delete;
        FrameBuffer& operator=(const FrameBuffer&) = delete;

        FrameBuffer(FrameBuffer&& other) noexcept
        {
            frame_buffer = other.frame_buffer;
            other.frame_buffer = VK_NULL_HANDLE;
        }

        FrameBuffer& operator=(FrameBuffer&& other) noexcept
        {
            frame_buffer = other.frame_buffer;
            other.frame_buffer = VK_NULL_HANDLE;
            return *this;
        }

        void release(VkDevice device, VkAllocationCallbacks* allocator = nullptr)
        {
            if (frame_buffer != VK_NULL_HANDLE)
            {
                vkDestroyFramebuffer(device, frame_buffer, allocator);
                frame_buffer = VK_NULL_HANDLE;
            }
        }

        inline operator VkFramebuffer() const noexcept
        {
            return frame_buffer;
        }
        inline const VkFramebuffer* operator&() const noexcept
        {
            return &frame_buffer;
        }

        inline VkFramebuffer handle() const noexcept
        {
            return frame_buffer;
        }
        inline const VkFramebuffer* handlePtr() const noexcept
        {
            return &frame_buffer;
        }

    private:
        VkFramebuffer frame_buffer;
    };

    class FrameBufferBuilder
    {
    public:
        FrameBufferBuilder()
        {
            info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            info.pNext = nullptr;
            info.flags = 0;
            info.renderPass = VK_NULL_HANDLE;
            info.attachmentCount = 0;
            info.pAttachments = nullptr;
            info.width = 0;
            info.height = 0;
            info.layers = 1;
        }

        FrameBufferBuilder& setRenderPass(VkRenderPass render_pass)
        {
            info.renderPass = render_pass;
            return *this;
        }

        FrameBufferBuilder& setAttachments(const VkImageView* attachments, uint32_t attachment_count)
        {
            info.attachmentCount = attachment_count;
            info.pAttachments = attachments;
            return *this;
        }

        FrameBufferBuilder& setSize(uint32_t width, uint32_t height)
        {
            info.width = width;
            info.height = height;
            return *this;
        }

        FrameBufferBuilder& setSize(VkExtent2D extent)
        {
            return setSize(extent.width, extent.height);
        }

        FrameBufferBuilder& setLayers(uint32_t layers)
        {
            info.layers = layers;
            return *this;
        }

        const VkFramebufferCreateInfo& getCreateInfo()
        {
            return info;
        }

        FrameBuffer build(VkDevice device, VkAllocationCallbacks* allocator = nullptr)
        {
            return FrameBuffer(device, info, allocator);
        }

    private:
        VkFramebufferCreateInfo info;
    };
}
