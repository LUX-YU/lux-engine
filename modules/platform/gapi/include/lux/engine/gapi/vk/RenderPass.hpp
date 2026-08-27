#pragma once
#include "lux/engine/gapi/vk/Object.hpp"
#include <vulkan/vulkan.h>
#include <vector>
#include <array>

namespace lux::gapi::vk
{
    class RenderPassBuilder;
    class RenderPass
    {
    public:
        using Builder = RenderPassBuilder;

        RenderPass() : render_pass(VK_NULL_HANDLE)
        {
        }

        RenderPass(VkDevice device, const VkRenderPassCreateInfo& info, VkAllocationCallbacks* allocator = nullptr)
        {
            VK_FUNC_INVOKE(
                vkCreateRenderPass,
                "Failed to create RenderPass object",
                device,
                &info,
                allocator,
                &render_pass
            );
        }

        RenderPass(const RenderPass&) = delete;
        RenderPass& operator=(const RenderPass&) = delete;

        RenderPass(RenderPass&& other) noexcept
        {
            render_pass = other.render_pass;
            other.render_pass = VK_NULL_HANDLE;
        }

        RenderPass& operator=(RenderPass&& other) noexcept
        {
            render_pass = other.render_pass;
            other.render_pass = VK_NULL_HANDLE;
            return *this;
        }

        void begin(
            VkFramebuffer framebuffer,
            VkCommandBuffer commandbuffer,
            const VkRect2D& area,
            VkSubpassContents subpass_contents = VkSubpassContents::VK_SUBPASS_CONTENTS_INLINE
        )
        {
            VkRenderPassBeginInfo begin_info{};
            begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            begin_info.renderPass = render_pass;
            begin_info.framebuffer = framebuffer;
            begin_info.renderArea = area;

            vkCmdBeginRenderPass(commandbuffer, &begin_info, subpass_contents);
        }

        void release(VkDevice device, VkAllocationCallbacks* allocator = nullptr)
        {
            if (render_pass != VK_NULL_HANDLE)
            {
                vkDestroyRenderPass(device, render_pass, allocator);
                render_pass = VK_NULL_HANDLE;
            }
        }

        inline operator VkRenderPass() const noexcept
        {
            return render_pass;
        }
        inline const VkRenderPass* operator&() const noexcept
        {
            return &render_pass;
        }

        inline VkRenderPass handle() const noexcept
        {
            return render_pass;
        }
        inline const VkRenderPass* handlePtr() const noexcept
        {
            return &render_pass;
        }

    private:
        VkRenderPass render_pass{VK_NULL_HANDLE};
    };

    class RenderPassBuilder
    {
    public:
        RenderPassBuilder()
        {
            create_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        }

        RenderPassBuilder& addAttachment(const VkAttachmentDescription& description)
        {
            attachments.push_back(description);
            return *this;
        }

        RenderPassBuilder& addAttachment(VkAttachmentDescription&& description)
        {
            attachments.emplace_back(std::move(description));
            return *this;
        }

        RenderPassBuilder& setAttachments(std::vector<VkAttachmentDescription> descriptions)
        {
            attachments = std::move(descriptions);
            return *this;
        }

        template <size_t N> RenderPassBuilder& setAttachments(const VkAttachmentDescription (&descriptions)[N])
        {
            attachments.assign(descriptions, descriptions + N);
            return *this;
        }

        template <size_t N>
        RenderPassBuilder& setAttachments(const std::array<VkAttachmentDescription, N>& descriptions)
        {
            attachments.assign(descriptions.begin(), descriptions.end());
            return *this;
        }

        RenderPassBuilder& addDependency(const VkSubpassDependency& dependency)
        {
            dependencies.push_back(dependency);
            return *this;
        }

        RenderPassBuilder& addDependency(VkSubpassDependency&& dependency)
        {
            dependencies.push_back(std::move(dependency));
            return *this;
        }

        RenderPassBuilder& setDependencies(std::vector<VkSubpassDependency> dependencies)
        {
            this->dependencies = std::move(dependencies);
            return *this;
        }

        template <size_t N> RenderPassBuilder& setDependencies(const VkSubpassDependency (&dependencies)[N])
        {
            this->dependencies.assign(dependencies, dependencies + N);
            return *this;
        }

        template <size_t N> RenderPassBuilder& setDependencies(const std::array<VkSubpassDependency, N>& dependencies)
        {
            this->dependencies.assign(dependencies.begin(), dependencies.end());
            return *this;
        }

        RenderPassBuilder& addSubpass(const VkSubpassDescription& subpasse)
        {
            subpasses.push_back(subpasse);
            return *this;
        }

        RenderPassBuilder& addSubpass(VkSubpassDescription&& subpasse)
        {
            subpasses.push_back(std::move(subpasse));
            return *this;
        }

        RenderPass build(VkDevice device, VkAllocationCallbacks* allocator = nullptr) const
        {
            create_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
            create_info.attachmentCount = static_cast<uint32_t>(attachments.size());
            create_info.pAttachments = attachments.data();
            create_info.subpassCount = static_cast<uint32_t>(subpasses.size());
            create_info.pSubpasses = subpasses.data();
            create_info.dependencyCount = static_cast<uint32_t>(dependencies.size());
            create_info.pDependencies = dependencies.data();

            return RenderPass(device, create_info, allocator);
        }

    private:
        std::vector<VkSubpassDescription> subpasses;
        std::vector<VkSubpassDependency> dependencies;
        std::vector<VkAttachmentDescription> attachments;
        mutable VkRenderPassCreateInfo create_info{};
    };
}
