#include <lux/engine/render/resources/ParticleResources.hpp>
#include <vk_mem_alloc.h>
#include <lux/engine/render/RendererContext.hpp>
#include <lux/engine/render/resources/lifecycle/VRAMBudgetGuard.hpp>
#include <cassert>

namespace lux::render
{

bool ParticleResources::init(const InitInfo& info)
{
    assert(info.device_ctx && "DeviceContext must be non-null");
    assert(info.max_particles > 0);
    assert(info.descriptor_pool != VK_NULL_HANDLE);
    assert(info.descriptor_layout != VK_NULL_HANDLE);

    dev_ctx_      = info.device_ctx;
    max_particles_= info.max_particles;
    desc_pool_    = info.descriptor_pool;
    desc_layout_  = info.descriptor_layout;

    // -----------------------------------------------------------------
    // 1. Allocate the GPU-only particle SSBO
    //    STORAGE_BUFFER_BIT for compute + vertex read/write.
    //    TRANSFER_DST_BIT for staging-buffer uploads.
    // -----------------------------------------------------------------
    {
        const VkDeviceSize buf_bytes =
            static_cast<VkDeviceSize>(max_particles_) * sizeof(ParticleGpu);

        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size        = buf_bytes;
        bci.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                        | VK_BUFFER_USAGE_TRANSFER_DST_BIT
                        | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;

        VkResult r = vmaCreateBuffer(dev_ctx_->vmaAllocator(), &bci, &aci,
                                     &particle_buf_.buf,
                                     &particle_buf_.alloc,
                                     nullptr);
        if (r != VK_SUCCESS)
        {
            return false;
        }
        particle_buf_.size = buf_bytes;
    }

    // -----------------------------------------------------------------
    // 2. Descriptor set allocation/write on provided layout.
    // -----------------------------------------------------------------
    if (!allocateDescriptorSet())  return false;
    updateDescriptorSet();

    initialized_ = true;
    return true;
}

void ParticleResources::destroy()
{
    if (!initialized_) return;

    desc_layout_ = VK_NULL_HANDLE;
    // desc_set_ is freed with the pool — no individual free needed.

    if (particle_buf_.buf != VK_NULL_HANDLE)
    {
        vmaDestroyBuffer(dev_ctx_->vmaAllocator(), particle_buf_.buf, particle_buf_.alloc);
        particle_buf_     = {};
    }

    initialized_ = false;
}

bool ParticleResources::allocateDescriptorSet()
{
    VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool     = desc_pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts        = &desc_layout_;

    VkResult r = vkAllocateDescriptorSets(dev_ctx_->logicalDevice(), &ai, &desc_set_);
    if (r != VK_SUCCESS)
    {
        return false;
    }
    return true;
}

void ParticleResources::updateDescriptorSet()
{
    VkDescriptorBufferInfo buf_info{};
    buf_info.buffer = particle_buf_.buf;
    buf_info.offset = 0;
    buf_info.range  = VK_WHOLE_SIZE;

    VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet          = desc_set_;
    w.dstBinding      = 0;
    w.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w.descriptorCount = 1;
    w.pBufferInfo     = &buf_info;

    vkUpdateDescriptorSets(dev_ctx_->logicalDevice(), 1, &w, 0, nullptr);
}

// =========================================================================
// tryExpand — grow particle SSBO at frame boundary.
// Records a copy command; old buffer is retired for deferred destruction.
// =========================================================================
bool ParticleResources::tryExpand(uint32_t new_max, VkCommandBuffer cmd)
{
    if (!initialized_ || new_max <= max_particles_) return false;

    // VRAM budget pre-check.
    const VkDeviceSize new_bytes =
        static_cast<VkDeviceSize>(new_max) * sizeof(ParticleGpu);
    const VkDeviceSize old_bytes = particle_buf_.size;
    {
        VRAMBudgetGuard guard(dev_ctx_->vmaAllocator());
        if (!guard.canAllocate(new_bytes - old_bytes))
            return false;
    }

    // 1. Allocate new GPU-only buffer.
    ParticleBuffer new_buf{};
    {
        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size        = new_bytes;
        bci.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                        | VK_BUFFER_USAGE_TRANSFER_DST_BIT
                        | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;

        VkResult r = vmaCreateBuffer(dev_ctx_->vmaAllocator(), &bci, &aci,
                                     &new_buf.buf, &new_buf.alloc, nullptr);
        if (r != VK_SUCCESS) return false;
        new_buf.size = new_bytes;
    }

    // 2. Copy old particle data → new buffer (preserves live particles).
    if (cmd != VK_NULL_HANDLE && old_bytes > 0)
    {
        VkBufferCopy region{};
        region.srcOffset = 0;
        region.dstOffset = 0;
        region.size      = old_bytes;
        vkCmdCopyBuffer(cmd, particle_buf_.buf, new_buf.buf, 1, &region);
    }

    // 3. Retire the old buffer.
    deferred_queue_->retireBuffer(particle_buf_.buf, particle_buf_.alloc);

    // 4. Swap in the new buffer.
    particle_buf_  = new_buf;
    max_particles_ = new_max;

    // 5. Update descriptor set to point at the new buffer.
    updateDescriptorSet();

    return true;
}

} // namespace lux::render

