#pragma once
#include <utility>
#include <memory>
#include <exception>
#include "lux/engine/gapi/vk/Object.hpp"
#include <vulkan/vulkan.h>

namespace lux::gapi::vk
{
    class ShaderModule
    {
    public:
        ShaderModule() noexcept : shader_module(VK_NULL_HANDLE) {}

        // Non-owning carrier. Its aggregate owner supplies the device at release.
        // Creation preserves an occupied carrier and never overwrites a live handle.
        [[nodiscard]] VkResult tryCreate(
            VkDevice device,
            const VkShaderModuleCreateInfo& info,
            const VkAllocationCallbacks* allocator = nullptr
        ) noexcept
        {
            if (shader_module != VK_NULL_HANDLE)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            VkShaderModule candidate{};
            const auto result = vkCreateShaderModule(device, &info, allocator, &candidate);
            if (result == VK_SUCCESS)
            {
                shader_module = candidate;
            }
            return result;
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
            if (this != std::addressof(other))
            {
                if (shader_module != VK_NULL_HANDLE)
                {
                    std::terminate(); // The aggregate owner must release the destination first.
                }
                shader_module = std::exchange(other.shader_module, VkShaderModule{});
            }
            return *this;
        }

        void release(VkDevice device, const VkAllocationCallbacks* allocator = nullptr) noexcept
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
        VkShaderModule shader_module{VK_NULL_HANDLE};
    };
}
