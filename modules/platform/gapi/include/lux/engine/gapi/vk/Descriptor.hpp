#pragma once
#include "Object.hpp"

namespace lux::gapi::vk
{
    // DescriptorSetLayout
    class DescriptorSetLayoutBuilder;
    class DescriptorSetLayout
    {
    public:
        DescriptorSetLayout() : descriptor_set_layout(VK_NULL_HANDLE)
        {
        }

        DescriptorSetLayout(VkDescriptorSetLayout layout) : descriptor_set_layout(layout)
        {
        }

        DescriptorSetLayout(
            VkDevice device,
            const VkDescriptorSetLayoutCreateInfo& info,
            VkAllocationCallbacks* allocator = nullptr
        )
        {
            VK_FUNC_INVOKE(
                vkCreateDescriptorSetLayout,
                "Failed to create DescriptorSetLayout object",
                device,
                &info,
                allocator,
                &descriptor_set_layout
            );
        }

        DescriptorSetLayout(const DescriptorSetLayout&) = delete;
        DescriptorSetLayout& operator=(const DescriptorSetLayout&) = delete;

        DescriptorSetLayout(DescriptorSetLayout&& other) noexcept
        {
            descriptor_set_layout = other.descriptor_set_layout;
            other.descriptor_set_layout = VK_NULL_HANDLE;
        }

        DescriptorSetLayout& operator=(DescriptorSetLayout&& other) noexcept
        {
            descriptor_set_layout = other.descriptor_set_layout;
            other.descriptor_set_layout = VK_NULL_HANDLE;
            return *this;
        }

        void release(VkDevice device, VkAllocationCallbacks* allocator = nullptr)
        {
            if (descriptor_set_layout != VK_NULL_HANDLE)
            {
                vkDestroyDescriptorSetLayout(device, descriptor_set_layout, allocator);
                descriptor_set_layout = VK_NULL_HANDLE;
            }
        }

        inline operator VkDescriptorSetLayout() const noexcept
        {
            return descriptor_set_layout;
        }
        inline const VkDescriptorSetLayout* operator&() const noexcept
        {
            return &descriptor_set_layout;
        }

        inline VkDescriptorSetLayout handle() const noexcept
        {
            return descriptor_set_layout;
        }
        inline const VkDescriptorSetLayout* handlePtr() const noexcept
        {
            return &descriptor_set_layout;
        }

    private:
        VkDescriptorSetLayout descriptor_set_layout{VK_NULL_HANDLE};
    };

    class DescriptorSetLayoutBuilder
    {
    public:
        DescriptorSetLayoutBuilder()
        {
            info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            info.pNext = nullptr;
            info.flags = 0;
        }

        DescriptorSetLayoutBuilder& setFlags(VkDescriptorSetLayoutCreateFlags flags)
        {
            info.flags = flags;
            return *this;
        }

        DescriptorSetLayoutBuilder& setBindings(const std::vector<VkDescriptorSetLayoutBinding>& b)
        {
            bindings = b;
            return *this;
        }

        DescriptorSetLayoutBuilder& setBindings(std::vector<VkDescriptorSetLayoutBinding>&& b)
        {
            bindings = std::move(b);
            return *this;
        }

        DescriptorSetLayoutBuilder& addBinding(const VkDescriptorSetLayoutBinding& binding)
        {
            bindings.push_back(binding);
            return *this;
        }

        DescriptorSetLayoutBuilder& setBindings(std::vector<VkDescriptorSetLayoutBinding> bindings)
        {
            this->bindings = std::move(bindings);
            return *this;
        }

        DescriptorSetLayout build(VkDevice device, VkAllocationCallbacks* allocator = nullptr) const
        {
            info.pBindings = bindings.data();
            info.bindingCount = static_cast<uint32_t>(bindings.size());
            return DescriptorSetLayout{device, info, allocator};
        }

    private:
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        mutable VkDescriptorSetLayoutCreateInfo info;
    };

    // DescriptorSet
    class DescriptorSetAllocator;
    class DescriptorSet
    {
    public:
        using Builder = DescriptorSetAllocator;

        DescriptorSet() : descriptor_set(VK_NULL_HANDLE)
        {
        }

        DescriptorSet(VkDevice device, const VkDescriptorSetAllocateInfo& info)
        {
            VK_FUNC_INVOKE(
                vkAllocateDescriptorSets,
                "Failed to allocate DescriptorSet object",
                device,
                &info,
                &descriptor_set
            );
        }

        DescriptorSet(const DescriptorSet&) = delete;
        DescriptorSet& operator=(const DescriptorSet&) = delete;

        DescriptorSet(DescriptorSet&& other) noexcept
        {
            descriptor_set = other.descriptor_set;
            other.descriptor_set = VK_NULL_HANDLE;
        }

        DescriptorSet& operator=(DescriptorSet&& other) noexcept
        {
            descriptor_set = other.descriptor_set;
            other.descriptor_set = VK_NULL_HANDLE;
            return *this;
        }

        void release(VkDevice device, VkDescriptorPool descriptor_pool)
        {
            if (descriptor_set != VK_NULL_HANDLE)
            {
                vkFreeDescriptorSets(device, descriptor_pool, 1, &descriptor_set);
                descriptor_set = VK_NULL_HANDLE;
            }
        }

        void
        update(VkDevice device, const VkDescriptorBufferInfo& buffer_info, uint32_t binding, uint32_t array_element = 0)
        {
            VkWriteDescriptorSet write_info = {
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                nullptr,
                descriptor_set,
                binding,
                array_element,
                1,
                VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                nullptr,
                &buffer_info,
                nullptr};

            vkUpdateDescriptorSets(device, 1, &write_info, 0, nullptr);
        }

        void free(VkDevice device, VkDescriptorPool descriptor_pool)
        {
            if (descriptor_set != VK_NULL_HANDLE)
            {
                vkFreeDescriptorSets(device, descriptor_pool, 1, &descriptor_set);
                descriptor_set = VK_NULL_HANDLE;
            }
        }

        inline operator VkDescriptorSet() const noexcept
        {
            return descriptor_set;
        }
        inline const VkDescriptorSet* operator&() const noexcept
        {
            return &descriptor_set;
        }

        inline VkDescriptorSet handle() const noexcept
        {
            return descriptor_set;
        }
        inline const VkDescriptorSet* handlePtr() const noexcept
        {
            return &descriptor_set;
        }

    private:
        VkDescriptorSet descriptor_set{VK_NULL_HANDLE};
    };

    class DescriptorSetAllocator
    {
    public:
        DescriptorSetAllocator()
        {
            info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            info.pNext = nullptr;
        }

        DescriptorSetAllocator& setDescriptorSetCount(uint32_t count)
        {
            info.descriptorSetCount = count;
            return *this;
        }

        DescriptorSetAllocator& setDescriptorPool(VkDescriptorPool pool)
        {
            info.descriptorPool = pool;
            return *this;
        }

        DescriptorSetAllocator& setSetLayouts(const VkDescriptorSetLayout* layouts)
        {
            info.pSetLayouts = layouts;
            return *this;
        }

        DescriptorSet allocate(VkDevice device) const
        {
            return DescriptorSet{device, info};
        }

    private:
        VkDescriptorSetAllocateInfo info;
    };

    // DescriptorPool
    class DescriptorPoolBuilder;
    class DescriptorPool
    {
    public:
        using Builder = DescriptorPoolBuilder;

        DescriptorPool() : descriptor_pool(VK_NULL_HANDLE)
        {
        }

        DescriptorPool(const VkDescriptorPoolCreateInfo& create_info, VkDevice device, VkAllocationCallbacks* allocator)
        {
            VK_FUNC_INVOKE(
                vkCreateDescriptorPool,
                "Failed to create DescriptorPool object",
                device,
                &create_info,
                allocator,
                &descriptor_pool
            );
        }

        DescriptorPool(const DescriptorPool&) = delete;
        DescriptorPool& operator=(const DescriptorPool&) = delete;

        DescriptorPool(DescriptorPool&& other) noexcept
        {
            descriptor_pool = other.descriptor_pool;
            other.descriptor_pool = VK_NULL_HANDLE;
        }

        DescriptorPool& operator=(DescriptorPool&& other) noexcept
        {
            descriptor_pool = other.descriptor_pool;
            other.descriptor_pool = VK_NULL_HANDLE;
            return *this;
        }

        void release(VkDevice device, VkAllocationCallbacks* allocator = nullptr)
        {
            if (descriptor_pool != VK_NULL_HANDLE)
            {
                vkDestroyDescriptorPool(device, descriptor_pool, allocator);
                descriptor_pool = VK_NULL_HANDLE;
            }
        }

        inline operator VkDescriptorPool() const noexcept
        {
            return descriptor_pool;
        }
        inline const VkDescriptorPool* operator&() const noexcept
        {
            return &descriptor_pool;
        }

        inline VkDescriptorPool handle() const noexcept
        {
            return descriptor_pool;
        }
        inline const VkDescriptorPool* handlePtr() const noexcept
        {
            return &descriptor_pool;
        }

        VkDescriptorPool descriptor_pool{VK_NULL_HANDLE};
    };

    class DescriptorPoolBuilder
    {
    public:
        DescriptorPoolBuilder()
        {
            // set default values
            create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            create_info.pNext = nullptr;
            create_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
            create_info.maxSets = 0;
            create_info.poolSizeCount = 0;
            create_info.pPoolSizes = nullptr;
        }

        // VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT or
        // VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT
        inline DescriptorPoolBuilder& setFlags(VkDescriptorPoolCreateFlags flags)
        {
            create_info.flags = flags;
            return *this;
        }

        inline DescriptorPoolBuilder& setMaxSets(uint32_t maxSets)
        {
            create_info.maxSets = maxSets;
            return *this;
        }

        inline DescriptorPoolBuilder& addPoolSize(VkDescriptorType type, uint32_t descriptorCount)
        {
            pool_sizes.emplace_back(VkDescriptorPoolSize{type, descriptorCount});
            return *this;
        }

        inline DescriptorPoolBuilder& addPoolSize(VkDescriptorPoolSize size)
        {
            pool_sizes.emplace_back(size);
            return *this;
        }

        inline DescriptorPoolBuilder& setPoolSizes(std::vector<VkDescriptorPoolSize> sizes)
        {
            pool_sizes = std::move(sizes);
            return *this;
        }

        DescriptorPool build(VkDevice device, VkAllocationCallbacks* allocator = nullptr)
        {
            create_info.poolSizeCount = (uint32_t)pool_sizes.size();
            create_info.pPoolSizes = pool_sizes.data();
            return DescriptorPool{create_info, device, allocator};
        }

    private:
        std::vector<VkDescriptorPoolSize> pool_sizes;
        VkDescriptorPoolCreateInfo create_info;
    };
}
