#include "skinning_skin_compute_comp_embed.hpp"
#include <lux/engine/render/gpu/VulkanContext.hpp>
#include <lux/engine/render/gpu/lifecycle/CommandBufferOwner.hpp>
#include <lux/engine/render/gpu/lifecycle/DeviceObject.hpp>
#include <lux/engine/render/gpu/memory/VmaTypes.hpp>
#include <lux/engine/render/gpu/pipeline/GeneralDescriptorSetLayout.hpp>
#include <lux/engine/render/resources/vertex/SkinningResources.hpp>
#include <lux/engine/render/resources/vertex/VertexPoolRegistry.hpp>
#include <vk_mem_alloc.h>

#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
    using namespace lux::render;

    VmaBuffer mappedBuffer(DeviceContext& device, VkDeviceSize size)
    {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size;
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        VmaAllocationCreateInfo allocation{};
        allocation.usage = VMA_MEMORY_USAGE_AUTO;
        allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
        auto buffer = VmaBuffer::create(device.vmaAllocator(), info, allocation);
        assert(buffer);
        return std::move(*buffer);
    }

    // A real mapped extension source: only its input data is fixture-owned.
    // Registration, descriptor publication, output backing and skinning shader are production code.
    class MappedSource final : public IVertexSource
    {
    public:
        explicit MappedSource(VmaBuffer backing) noexcept : backing_(std::move(backing)) {}

        EVertexSourceKind kind() const noexcept override
        {
            return EVertexSourceKind::STATIC_POOL;
        }

        VkBuffer buffer() const noexcept override
        {
            return backing_.buffer();
        }

        VertexLayoutId layout() const noexcept override
        {
            return 0;
        }

        std::uint32_t bindlessPoolId() const noexcept override
        {
            return pool_;
        }

        void setBindlessPoolId(std::uint32_t pool) noexcept override
        {
            pool_ = pool;
        }

    private:
        VmaBuffer backing_;
        std::uint32_t pool_{~0u};
    };

    void executeSkinning(DeviceContext& device)
    {
        const VkDevice native = device.logicalDevice();
        auto resources = ResourceContext::create(device);
        auto layouts = GeneralDescriptorSetLayout::create(device);
        assert(resources && layouts);
        std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
        for (std::uint32_t index = 0; index < bindings.size(); ++index)
        {
            bindings[index] = {index, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        }
        VkDescriptorSetLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layout_info.bindingCount = static_cast<std::uint32_t>(bindings.size());
        layout_info.pBindings = bindings.data();
        auto skin_layout = DescriptorSetLayoutOwner::create(native, layout_info);
        assert(skin_layout);
        const std::array set_layouts{
            skin_layout->get(),
            skin_layout->get(),
            (*layouts)->getLayout(EDescriptorSetSlot::VERTEX_POOL)
        };
        const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kVertexPoolMaxCount + 6};
        VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        pool_info.maxSets = 3;
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes = &size;
        auto pool = DescriptorPoolOwner::create(native, pool_info);
        assert(pool);
        std::array<VkDescriptorSet, 3> sets{};
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = pool->get();
        allocate.descriptorSetCount = static_cast<std::uint32_t>(sets.size());
        allocate.pSetLayouts = set_layouts.data();
        assert(vkAllocateDescriptorSets(native, &allocate, sets.data()) == VK_SUCCESS);
        const std::array targets{sets[2]};
        auto registry = VertexPoolRegistry::create(device, targets, 0);
        assert(registry);

        std::array<float, 44> vertices{};
        for (unsigned index = 0; index < 2; ++index)
        {
            vertices[index * 22] = float(index + 1);
            vertices[index * 22 + 1] = 2.f;
            vertices[index * 22 + 2] = -3.f;
            vertices[index * 22 + 4] = 1.f;
            vertices[index * 22 + 6] = 1.f;
            vertices[index * 22 + 13] = 1.f;
            vertices[index * 22 + 18] = 1.f; // First bone index bits are zero, weight=1.
        }
        auto input = mappedBuffer(device, sizeof(vertices));
        auto* input_bytes = input.map();
        assert(input_bytes);
        std::memcpy(input_bytes, vertices.data(), sizeof(vertices));
        input.flush();
        input.unmap();
        MappedSource source(std::move(input));
        auto registration = (*registry)->registerSource(source);
        assert(registration && registration->poolId() == 0);
        VertexSourceRegistration input_registration(std::move(*registration));
        assert(!*registration && input_registration.poolId() == 0);
        SkinningResources::CreateInfo skin_info{};
        skin_info.device_context = &device;
        skin_info.vertex_pool_registry = registry->get();
        skin_info.layout_id = 0;
        skin_info.vertex_stride = 88;
        skin_info.max_bones = 1;
        skin_info.max_dispatches = 1;
        skin_info.output_pool_bytes = 88 * 64;
        auto skin = SkinningResources::create(skin_info);
        assert(skin && (*skin)->outputPoolId() == 1);
        auto readback = mappedBuffer(device, sizeof(vertices));
        auto read_params = mappedBuffer(device, sizeof(SkinDispatchParams));

        const std::array pipeline_sets{set_layouts[0], set_layouts[2]};
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(std::uint32_t)};
        VkPipelineLayoutCreateInfo pipeline_layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipeline_layout_info.setLayoutCount = static_cast<std::uint32_t>(pipeline_sets.size());
        pipeline_layout_info.pSetLayouts = pipeline_sets.data();
        pipeline_layout_info.pushConstantRangeCount = 1;
        pipeline_layout_info.pPushConstantRanges = &push;
        auto pipeline_layout = PipelineLayoutOwner::create(native, pipeline_layout_info);
        assert(pipeline_layout);
        VkShaderModuleCreateInfo shader_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shader_info.codeSize = builtin::skinning_skin_compute_comp_spirv_size;
        shader_info.pCode = reinterpret_cast<const std::uint32_t*>(builtin::skinning_skin_compute_comp_spirv);
        auto shader = ShaderModuleOwner::create(native, shader_info);
        assert(shader);
        VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipeline_info.stage.module = shader->get();
        pipeline_info.stage.pName = "main";
        pipeline_info.layout = pipeline_layout->get();
        auto pipeline = ComputePipelineOwner::create(native, pipeline_info);
        assert(pipeline);

        for (unsigned frame = 0; frame < 2; ++frame)
        {
            (*skin)->beginFrameIfNew(frame);
            BoneMatrixGpu bone{};
            bone.m[0] = bone.m[5] = bone.m[10] = bone.m[15] = 1.f;
            bone.m[12] = 10.f + float(frame);
            assert((*skin)->uploadBonePalette(&bone, 1) == 0);
            auto output = (*skin)->queueDispatch(input_registration.poolId(), 0, 2, 0, 1);
            assert(output.valid() && output.pool_id == 1 && (*skin)->uploadDispatches() == 1);
            const SkinDispatchParams copy{0, 2, output.vertex_base, 0, 0, output.pool_id};
            auto* parameters = read_params.map();
            assert(parameters);
            std::memcpy(parameters, &copy, sizeof(copy));
            read_params.flush();
            read_params.unmap();
            const std::array buffers{
                (*skin)->bonePaletteBuffer(frame),
                (*skin)->outputPool().buffer(),
                (*skin)->dispatchParamsBuffer(frame),
                (*skin)->bonePaletteBuffer(frame),
                readback.buffer(),
                read_params.buffer()
            };
            std::array<VkDescriptorBufferInfo, 6> descriptors{};
            std::array<VkWriteDescriptorSet, 6> writes{};
            for (unsigned index = 0; index < writes.size(); ++index)
            {
                descriptors[index] = {buffers[index], 0, VK_WHOLE_SIZE};
                writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[index].dstSet = sets[index / 3];
                writes[index].dstBinding = index % 3;
                writes[index].descriptorCount = 1;
                writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[index].pBufferInfo = &descriptors[index];
            }
            vkUpdateDescriptorSets(native, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
            auto command = CommandBufferOwner::create(native, (*resources)->commandPool());
            assert(command);
            const auto cmd = command->get();
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            assert(vkBeginCommandBuffer(cmd, &begin) == VK_SUCCESS);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->get());
            const std::uint32_t count = 1;
            vkCmdPushConstants(cmd, pipeline_layout->get(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(count), &count);
            for (unsigned pass = 0; pass < 2; ++pass)
            {
                const std::array bound{sets[pass], sets[2]};
                vkCmdBindDescriptorSets(
                    cmd,
                    VK_PIPELINE_BIND_POINT_COMPUTE,
                    pipeline_layout->get(),
                    0,
                    static_cast<std::uint32_t>(bound.size()),
                    bound.data(),
                    0,
                    nullptr
                );
                vkCmdDispatch(cmd, 1, 1, 1);
                VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
                barrier.dstStageMask =
                    pass == 0 ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_2_HOST_BIT;
                barrier.dstAccessMask = pass == 0 ? VK_ACCESS_2_SHADER_READ_BIT : VK_ACCESS_2_HOST_READ_BIT;
                VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dependency.memoryBarrierCount = 1;
                dependency.pMemoryBarriers = &barrier;
                vkCmdPipelineBarrier2(cmd, &dependency);
            }
            assert(vkEndCommandBuffer(cmd) == VK_SUCCESS);
            VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            auto fence = FenceOwner::create(native, fence_info);
            assert(fence);
            VkSubmitInfo submission{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submission.commandBufferCount = 1;
            submission.pCommandBuffers = &cmd;
            assert(vkQueueSubmit(device.graphicsQueue(), 1, &submission, fence->get()) == VK_SUCCESS);
            // Registrations and native backing remain alive until the actual fence, never a frame-count guess.
            assert(input_registration && (*registry)->isRegistered(output.pool_id));
            const auto completion = fence->get();
            assert(vkWaitForFences(native, 1, &completion, VK_TRUE, UINT64_MAX) == VK_SUCCESS);
            assert(
                vmaInvalidateAllocation(device.vmaAllocator(), readback.allocation(), 0, VK_WHOLE_SIZE) == VK_SUCCESS
            );
            const auto* values = static_cast<const float*>(readback.map());
            assert(values);
            for (unsigned index = 0; index < 2; ++index)
            {
                assert(std::abs(values[index * 22] - (vertices[index * 22] + bone.m[12])) < 0.0001f);
                assert(values[index * 22 + 1] == 2.f && values[index * 22 + 2] == -3.f);
                assert(values[index * 22 + 18] == 0.f);
            }
            readback.unmap();
        }
        skin->reset();
        assert(!(*registry)->isRegistered(1) && (*registry)->isRegistered(0));
        input_registration = {};
        assert(source.bindlessPoolId() == ~0u && !(*registry)->isRegistered(0));
    }
} // namespace

int main()
{
    std::atomic<unsigned> errors{};
    {
        auto instance = lux::render::InstanceContext::create(
            {},
            [&errors](const lux::render::DebugCallbackInfo& info)
            {
                if (info.flags & VK_DEBUG_REPORT_ERROR_BIT_EXT)
                {
                    ++errors;
                    std::fprintf(stderr, "%s\n", info.message);
                }
                return false;
            }
        );
        assert(instance);
        // InstanceContext enables these only with the actual Khronos validation layer.
        assert((*instance)->isInstanceExtensionEnabled(VK_EXT_DEBUG_REPORT_EXTENSION_NAME));
        assert((*instance)->isInstanceExtensionEnabled(VK_EXT_DEBUG_UTILS_EXTENSION_NAME));
        auto device = lux::render::DeviceContext::create(
            **instance,
            lux::render::EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED
        );
        assert(device);
        executeSkinning(**device);
    }
    assert(errors == 0);
    std::puts("Actual production skin_compute: two frames, input/output registered pools, transformed GPU readback, "
              "fence-before-revoke and zero validation errors PASS");
}
