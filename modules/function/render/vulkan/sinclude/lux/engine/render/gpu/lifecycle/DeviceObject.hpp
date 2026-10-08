#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <vulkan/vulkan.h>

#include <concepts>
#include <utility>

namespace lux::render
{
    namespace detail
    {
        // PipelineManager creates one pipeline per call, without a native pipeline cache.
        // The single-output contract lets the regular device owner keep the same factory shape.
        template <auto Create, class Info>
        VkResult createSinglePipeline(
            VkDevice device,
            const Info* info,
            const VkAllocationCallbacks* allocator,
            VkPipeline* output
        ) noexcept
        {
            return Create(device, VK_NULL_HANDLE, 1, info, allocator, output);
        }

        template <class Handle, class Info, auto Create, auto Destroy>
        concept DeviceObjectApi = requires(VkDevice device, const Info* info, Handle* output, Handle handle) {
            { Create(device, info, nullptr, output) } -> std::same_as<VkResult>;
            { Destroy(device, handle, nullptr) } -> std::same_as<void>;
        };

        /// Owns one native object with the exact allocation callbacks used at creation.
        /// Device and optional callback context are borrowed and must outlive the owner.
        /// get() never transfers ownership.
        /// Destruction requires a GPU-safe point: in-flight objects must be moved as owners
        /// into the existing retirement container before their semantic owner disappears.
        template <class Handle, class Info, auto Create, auto Destroy>
            requires DeviceObjectApi<Handle, Info, Create, Destroy>
        class TDeviceObject final
        {
        public:
            using CreateResult = lux::cxx::expected<TDeviceObject, VkResult>;

            [[nodiscard]] static CreateResult
            create(VkDevice device, const Info& info, const VkAllocationCallbacks* allocator = nullptr) noexcept
            {
                Handle handle{};
                const auto result = Create(device, &info, allocator, &handle);
                if (result != VK_SUCCESS)
                {
                    return lux::cxx::unexpected(result);
                }
                return TDeviceObject(device, handle, allocator);
            }

            TDeviceObject() noexcept = default;

            ~TDeviceObject() noexcept
            {
                reset();
            }

            TDeviceObject(const TDeviceObject&) = delete;
            TDeviceObject& operator=(const TDeviceObject&) = delete;

            TDeviceObject(TDeviceObject&& other) noexcept
                : device_(std::exchange(other.device_, {})), handle_(std::exchange(other.handle_, {})),
                  allocator_(std::exchange(other.allocator_, nullptr))
            {
            }

            TDeviceObject& operator=(TDeviceObject&& other) noexcept
            {
                if (this != &other)
                {
                    reset();
                    device_ = std::exchange(other.device_, {});
                    handle_ = std::exchange(other.handle_, {});
                    allocator_ = std::exchange(other.allocator_, nullptr);
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
                    Destroy(device_, handle_, allocator_);
                    handle_ = VK_NULL_HANDLE;
                    device_ = VK_NULL_HANDLE;
                    allocator_ = nullptr;
                }
            }

        private:
            TDeviceObject(VkDevice device, Handle handle, const VkAllocationCallbacks* allocator) noexcept
                : device_(device), handle_(handle), allocator_(allocator)
            {
            }

            VkDevice device_{};
            Handle handle_{};
            const VkAllocationCallbacks* allocator_{};
        };
    } // namespace detail

    using SamplerOwner = detail::TDeviceObject<VkSampler, VkSamplerCreateInfo, vkCreateSampler, vkDestroySampler>;
    using DescriptorSetLayoutOwner = detail::TDeviceObject<
        VkDescriptorSetLayout,
        VkDescriptorSetLayoutCreateInfo,
        vkCreateDescriptorSetLayout,
        vkDestroyDescriptorSetLayout
    >;
    using PipelineLayoutOwner =
        detail::TDeviceObject<VkPipelineLayout, VkPipelineLayoutCreateInfo, vkCreatePipelineLayout, vkDestroyPipelineLayout>;
    using DescriptorPoolOwner =
        detail::TDeviceObject<VkDescriptorPool, VkDescriptorPoolCreateInfo, vkCreateDescriptorPool, vkDestroyDescriptorPool>;
    using ShaderModuleOwner =
        detail::TDeviceObject<VkShaderModule, VkShaderModuleCreateInfo, vkCreateShaderModule, vkDestroyShaderModule>;
    using SemaphoreOwner =
        detail::TDeviceObject<VkSemaphore, VkSemaphoreCreateInfo, vkCreateSemaphore, vkDestroySemaphore>;
    using CommandPoolOwner =
        detail::TDeviceObject<VkCommandPool, VkCommandPoolCreateInfo, vkCreateCommandPool, vkDestroyCommandPool>;
    using ImageViewOwner =
        detail::TDeviceObject<VkImageView, VkImageViewCreateInfo, vkCreateImageView, vkDestroyImageView>;
    using QueryPoolOwner =
        detail::TDeviceObject<VkQueryPool, VkQueryPoolCreateInfo, vkCreateQueryPool, vkDestroyQueryPool>;
    using FenceOwner = detail::TDeviceObject<VkFence, VkFenceCreateInfo, vkCreateFence, vkDestroyFence>;
    using RenderPassOwner =
        detail::TDeviceObject<VkRenderPass, VkRenderPassCreateInfo, vkCreateRenderPass, vkDestroyRenderPass>;
    using GraphicsPipelineOwner = detail::TDeviceObject<
        VkPipeline,
        VkGraphicsPipelineCreateInfo,
        detail::createSinglePipeline<vkCreateGraphicsPipelines, VkGraphicsPipelineCreateInfo>,
        vkDestroyPipeline
    >;
    using ComputePipelineOwner = detail::TDeviceObject<
        VkPipeline,
        VkComputePipelineCreateInfo,
        detail::createSinglePipeline<vkCreateComputePipelines, VkComputePipelineCreateInfo>,
        vkDestroyPipeline
    >;
} // namespace lux::render
