#pragma once
#include "lux/engine/gapi/vk/Object.hpp"
#include <vulkan/vulkan.h>

namespace lux::gapi::vk
{
    class ShaderModule
    {
    public:
        ShaderModule() : shader_module(VK_NULL_HANDLE)
        {
        }

        ShaderModule(VkDevice device, const VkShaderModuleCreateInfo& info, VkAllocationCallbacks* allocator = nullptr)
        {
            VK_FUNC_INVOKE(
                vkCreateShaderModule,
                "Failed to create ShaderModule object",
                device,
                &info,
                allocator,
                &shader_module
            );
        }

        ShaderModule(const ShaderModule&) = delete;
        ShaderModule& operator=(const ShaderModule&) = delete;

        ShaderModule(ShaderModule&& other) noexcept
        {
            shader_module = other.shader_module;
            other.shader_module = VK_NULL_HANDLE;
        }

        ShaderModule& operator=(ShaderModule&& other) noexcept
        {
            shader_module = other.shader_module;
            other.shader_module = VK_NULL_HANDLE;
            return *this;
        }

        void release(VkDevice device, VkAllocationCallbacks* allocator = nullptr)
        {
            if (shader_module != VK_NULL_HANDLE)
            {
                vkDestroyShaderModule(device, shader_module, allocator);
                shader_module = VK_NULL_HANDLE;
            }
        }

        inline operator VkShaderModule() const noexcept
        {
            return shader_module;
        }
        inline const VkShaderModule* operator&() const noexcept
        {
            return &shader_module;
        }

        inline VkShaderModule handle() const noexcept
        {
            return shader_module;
        }
        inline const VkShaderModule* handlePtr() const noexcept
        {
            return &shader_module;
        }

    private:
        VkShaderModule shader_module;
    };
}
