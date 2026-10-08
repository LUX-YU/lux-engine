#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <vulkan/vulkan.h>

#include <concepts>
#include <utility>

namespace lux::render
{
    namespace detail
    {
        template <class Handle, class Info, auto Create, auto Destroy>
        concept DeviceObjectApi = requires(VkDevice device, const Info* info, Handle* output, Handle handle) {
            { Create(device, info, nullptr, output) } -> std::same_as<VkResult>;
            { Destroy(device, handle, nullptr) } -> std::same_as<void>;
        };

        /// Owns one native object allocated with Vulkan's default allocation callbacks.
        /// Device is borrowed and must outlive the owner. get() never transfers ownership.
        /// Destruction requires a GPU-safe point: in-flight objects must be moved as owners
        /// into the existing retirement container before their semantic owner disappears.
        template <class Handle, class Info, auto Create, auto Destroy>
            requires DeviceObjectApi<Handle, Info, Create, Destroy>
        class TDeviceObject final
        {
        public:
            using CreateResult = lux::cxx::expected<TDeviceObject, VkResult>;

            [[nodiscard]] static CreateResult create(VkDevice device, const Info& info) noexcept
            {
                Handle handle{};
                const auto result = Create(device, &info, nullptr, &handle);
                if (result != VK_SUCCESS)
                {
                    return lux::cxx::unexpected(result);
                }
                return TDeviceObject(device, handle);
            }

            TDeviceObject() noexcept = default;

            ~TDeviceObject() noexcept
            {
                reset();
            }

            TDeviceObject(const TDeviceObject&) = delete;
            TDeviceObject& operator=(const TDeviceObject&) = delete;

            TDeviceObject(TDeviceObject&& other) noexcept
                : device_(std::exchange(other.device_, {})), handle_(std::exchange(other.handle_, {}))
            {
            }

            TDeviceObject& operator=(TDeviceObject&& other) noexcept
            {
                if (this != &other)
                {
                    reset();
                    device_ = std::exchange(other.device_, {});
                    handle_ = std::exchange(other.handle_, {});
                }
                return *this;
            }

            [[nodiscard]] Handle get() const noexcept
            {
                return handle_;
            }

            explicit operator bool() const noexcept
            {
                return handle_ != VK_NULL_HANDLE;
            }

            void reset() noexcept
            {
                if (handle_ != VK_NULL_HANDLE)
                {
                    Destroy(device_, handle_, nullptr);
                    handle_ = VK_NULL_HANDLE;
                    device_ = VK_NULL_HANDLE;
                }
            }

        private:
            TDeviceObject(VkDevice device, Handle handle) noexcept : device_(device), handle_(handle) {}

            VkDevice device_{};
            Handle handle_{};
        };
    } // namespace detail

    using SamplerOwner = detail::TDeviceObject<VkSampler, VkSamplerCreateInfo, vkCreateSampler, vkDestroySampler>;
    using DescriptorSetLayoutOwner = detail::TDeviceObject<
        VkDescriptorSetLayout,
        VkDescriptorSetLayoutCreateInfo,
        vkCreateDescriptorSetLayout,
        vkDestroyDescriptorSetLayout>;
    using PipelineLayoutOwner = detail::
        TDeviceObject<VkPipelineLayout, VkPipelineLayoutCreateInfo, vkCreatePipelineLayout, vkDestroyPipelineLayout>;
    using DescriptorPoolOwner = detail::
        TDeviceObject<VkDescriptorPool, VkDescriptorPoolCreateInfo, vkCreateDescriptorPool, vkDestroyDescriptorPool>;
    using ShaderModuleOwner = detail::
        TDeviceObject<VkShaderModule, VkShaderModuleCreateInfo, vkCreateShaderModule, vkDestroyShaderModule>;
    using SemaphoreOwner = detail::TDeviceObject<VkSemaphore, VkSemaphoreCreateInfo, vkCreateSemaphore, vkDestroySemaphore>;
    using CommandPoolOwner = detail::
        TDeviceObject<VkCommandPool, VkCommandPoolCreateInfo, vkCreateCommandPool, vkDestroyCommandPool>;
    using ImageViewOwner = detail::TDeviceObject<VkImageView, VkImageViewCreateInfo, vkCreateImageView, vkDestroyImageView>;
} // namespace lux::render
