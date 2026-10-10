#include <lux/engine/render/vulkan/descriptor/Descriptors.hpp>
#include <lux/engine/render/vulkan/pipeline/Pipeline.hpp>

#include "Native.hpp"
#include <utility>

namespace lux::render::vulkan
{
    DescriptorSetLayout::DescriptorSetLayout(VkDevice device, VkDescriptorSetLayout handle) noexcept
        : device_(device), handle_(handle)
    {
    }

    void DescriptorSetLayout::release() noexcept
    {
        if (handle_)
        {
            LUX_DESTROY("descriptor_layout", vkDestroyDescriptorSetLayout(device_, handle_, nullptr));
        }
    }

    DescriptorSetLayout::~DescriptorSetLayout() noexcept
    {
        release();
    }

    DescriptorSetLayout::DescriptorSetLayout(DescriptorSetLayout&& other) noexcept
        : device_(other.device_), handle_(std::exchange(other.handle_, VK_NULL_HANDLE))
    {
    }

    DescriptorSetLayout& DescriptorSetLayout::operator=(DescriptorSetLayout&& other) noexcept
    {
        if (this != &other)
        {
            release();
            device_ = other.device_;
            handle_ = std::exchange(other.handle_, VK_NULL_HANDLE);
        }
        return *this;
    }

    DescriptorPool::DescriptorPool(
        const VulkanDevice& device,
        VkDescriptorPool handle,
        std::uint32_t remaining
    ) noexcept
        : device_(device.native()), handle_(handle),
          storage_alignment_(device.properties().limits.minStorageBufferOffsetAlignment),
          storage_range_(device.properties().limits.maxStorageBufferRange),
          uniform_alignment_(device.properties().limits.minUniformBufferOffsetAlignment),
          uniform_range_(device.properties().limits.maxUniformBufferRange), remaining_sets_(remaining)
    {
    }

    void DescriptorPool::release() noexcept
    {
        if (handle_)
        {
            LUX_DESTROY("descriptor_pool", vkDestroyDescriptorPool(device_, handle_, nullptr));
        }
    }

    DescriptorPool::~DescriptorPool() noexcept
    {
        release();
    }

    DescriptorPool::DescriptorPool(DescriptorPool&& other) noexcept
        : device_(other.device_), handle_(std::exchange(other.handle_, VK_NULL_HANDLE)),
          storage_alignment_(other.storage_alignment_), storage_range_(other.storage_range_),
          uniform_alignment_(other.uniform_alignment_), uniform_range_(other.uniform_range_),
          remaining_sets_(other.remaining_sets_)
    {
    }

    DescriptorPool& DescriptorPool::operator=(DescriptorPool&& other) noexcept
    {
        if (this != &other)
        {
            release();
            device_ = other.device_;
            handle_ = std::exchange(other.handle_, VK_NULL_HANDLE);
            storage_alignment_ = other.storage_alignment_;
            storage_range_ = other.storage_range_;
            uniform_alignment_ = other.uniform_alignment_;
            uniform_range_ = other.uniform_range_;
            remaining_sets_ = other.remaining_sets_;
        }
        return *this;
    }

    ShaderModule::ShaderModule(VkDevice device, VkShaderModule handle) noexcept : device_(device), handle_(handle) {}

    void ShaderModule::release() noexcept
    {
        if (handle_)
        {
            LUX_DESTROY("shader", vkDestroyShaderModule(device_, handle_, nullptr));
        }
    }

    ShaderModule::~ShaderModule() noexcept
    {
        release();
    }

    ShaderModule::ShaderModule(ShaderModule&& other) noexcept
        : device_(other.device_), handle_(std::exchange(other.handle_, VK_NULL_HANDLE))
    {
    }

    ShaderModule& ShaderModule::operator=(ShaderModule&& other) noexcept
    {
        if (this != &other)
        {
            release();
            device_ = other.device_;
            handle_ = std::exchange(other.handle_, VK_NULL_HANDLE);
        }
        return *this;
    }

    PipelineLayout::PipelineLayout(VkDevice device, VkPipelineLayout handle) noexcept : device_(device), handle_(handle)
    {
    }

    void PipelineLayout::release() noexcept
    {
        if (handle_)
        {
            LUX_DESTROY("pipeline_layout", vkDestroyPipelineLayout(device_, handle_, nullptr));
        }
    }

    PipelineLayout::~PipelineLayout() noexcept
    {
        release();
    }

    PipelineLayout::PipelineLayout(PipelineLayout&& other) noexcept
        : device_(other.device_), handle_(std::exchange(other.handle_, VK_NULL_HANDLE))
    {
    }

    PipelineLayout& PipelineLayout::operator=(PipelineLayout&& other) noexcept
    {
        if (this != &other)
        {
            release();
            device_ = other.device_;
            handle_ = std::exchange(other.handle_, VK_NULL_HANDLE);
        }
        return *this;
    }

    ComputePipeline::ComputePipeline(VkDevice device, VkPipeline handle) noexcept : device_(device), handle_(handle) {}

    void ComputePipeline::release() noexcept
    {
        if (handle_)
        {
            LUX_DESTROY("pipeline", vkDestroyPipeline(device_, handle_, nullptr));
        }
    }

    ComputePipeline::~ComputePipeline() noexcept
    {
        release();
    }

    ComputePipeline::ComputePipeline(ComputePipeline&& other) noexcept
        : device_(other.device_), handle_(std::exchange(other.handle_, VK_NULL_HANDLE))
    {
    }

    ComputePipeline& ComputePipeline::operator=(ComputePipeline&& other) noexcept
    {
        if (this != &other)
        {
            release();
            device_ = other.device_;
            handle_ = std::exchange(other.handle_, VK_NULL_HANDLE);
        }
        return *this;
    }

} // namespace lux::render::vulkan
