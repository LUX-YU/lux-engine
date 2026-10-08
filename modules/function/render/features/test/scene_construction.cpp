#define LUX_SCENE_NATIVE_FAULTS
#include "shadow_native.hpp"

#include <lux/engine/render/gpu/pipeline/GeneralDescriptorSetLayout.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineManager.hpp>

#include <cstdio>
#include <cstring>
#include <lux/engine/render/gpu/lifecycle/CommandBufferOwner.hpp>
#include <type_traits>

struct LifetimeCounts
{
    unsigned allocated{}, released{}, detached{}, destroyed{};
};

class LifetimeFeature final : public lux::render::RenderFeature
{
public:
    explicit LifetimeFeature(LifetimeCounts& counts) : RenderFeature(Config{"lifetime-test"}), counts_(counts) {}

    ~LifetimeFeature() override
    {
        assert(counts_.detached == 1);
        ++counts_.destroyed;
    }

    bool allocateViewState(uint32_t, lux::render::RenderScene& scene) override
    {
        scene_ = &scene;
        ++counts_.allocated;
        return true;
    }

    void deallocateViewState(uint32_t) override
    {
        assert(scene_->resources().find<lux::render::SceneResources>());
        ++counts_.released;
    }

    void onDetachFromScene(lux::render::RenderScene& scene) override
    {
        assert(counts_.released == counts_.allocated);
        assert(scene.resources().find<lux::render::SceneResources>());
        ++counts_.detached;
    }

private:
    LifetimeCounts& counts_;
    lux::render::RenderScene* scene_{};
};

int main()
{
    using namespace lux::render;
    using Boundary = shadow_fault::EBoundary;
    static_assert(!std::is_default_constructible_v<TransferScheduler>);
    static_assert(!std::is_copy_constructible_v<TransferScheduler>);
    static_assert(std::is_nothrow_move_constructible_v<TransferScheduler>);
    static_assert(!std::is_constructible_v<RenderScene, std::shared_ptr<RenderContext>>);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    auto instance_owner = InstanceContext::create({});
    assert(instance_owner);
    auto& instance = **instance_owner;
    DeviceContext device(instance);
    assert(device.init(EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED));
    auto resources = ResourceContext::create(device);
    assert(resources);
    const auto baseline_pools = shadow_fault::live_pools.size();
    const auto makeContext = [&]()
    {
        auto layouts = GeneralDescriptorSetLayout::create(device);
        assert(layouts);
        RenderContext::CreateInfo info{
            std::make_unique<PipelineManager>(device, true),
            std::move(*layouts),
            std::make_unique<ResourceRegistry>(),
            2
        };
        return RenderContext::create(**resources, std::move(info));
    };
    for (auto boundary : {Boundary::BUFFER, Boundary::MAPPING})
    {
        shadow_fault::boundary = boundary;
        const auto rejected = makeContext();
        assert(!rejected && isError<err::device::VulkanCallFailed>(rejected.error()));
        const auto expected = boundary == Boundary::BUFFER ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_ERROR_MEMORY_MAP_FAILED;
        assert(rejected.error().args[0] == encodeVkResult(expected));
        assert(shadow_fault::live_buffers.empty());
    }
    shadow_fault::boundary = Boundary::NONE;
    auto context = makeContext();
    assert(context);
    auto& queue = (*context)->deferredDestroyQueue();
    const auto baseline = shadow_fault::live_buffers.size();
    assert(baseline == 1);
    assert(!RenderScene::create({}));
    shadow_fault::attempts.fill(0);
    {
        const auto candidate = RenderScene::create(*context);
        assert(candidate);
        assert((*candidate)->transferScheduler().allocateStaging(256));
    }
    const auto counts = shadow_fault::attempts;
    queue.flushAll();
    assert(shadow_fault::live_buffers.size() == baseline && shadow_fault::live_pools.size() == baseline_pools);
    for (auto boundary : {Boundary::POOL, Boundary::SET, Boundary::BUFFER, Boundary::MAPPING, Boundary::FLUSH})
    {
        const auto count = counts[static_cast<unsigned>(boundary)];
        assert(count != 0);
        for (unsigned index = 0; index != count; ++index)
        {
            shadow_fault::boundary = boundary;
            shadow_fault::skip = index;
            const auto rejected_before = shadow_fault::rejected;
            const auto candidate = RenderScene::create(*context);
            assert(!candidate && isError<err::device::VulkanCallFailed>(candidate.error()));
            const auto expected = boundary == Boundary::MAPPING || boundary == Boundary::FLUSH
                                      ? VK_ERROR_MEMORY_MAP_FAILED
                                      : VK_ERROR_OUT_OF_DEVICE_MEMORY;
            assert(candidate.error().args[0] == encodeVkResult(expected));
            assert(shadow_fault::rejected == rejected_before + 1);
            shadow_fault::boundary = Boundary::NONE;
            queue.flushAll();
            assert(shadow_fault::live_buffers.size() == baseline && shadow_fault::live_pools.size() == baseline_pools);
        }
        std::printf(
            "Scene complete construction: native boundary=%u count=%u PASS\n",
            static_cast<unsigned>(boundary),
            count
        );
    }
    {
        auto created = TransferScheduler::create({device.vmaAllocator(), 256, 2});
        assert(created);
        auto scheduler = std::move(*created);
        assert(!scheduler.allocateStaging(0));
        const auto before = shadow_fault::live_buffers.size();
        for (auto boundary : {Boundary::BUFFER, Boundary::MAPPING})
        {
            shadow_fault::boundary = boundary;
            assert(!scheduler.allocateStaging(1024));
            assert(shadow_fault::live_buffers.size() == before);
        }
        shadow_fault::boundary = Boundary::NONE;
        auto overflow = scheduler.allocateStaging(1024);
        assert(overflow && shadow_fault::live_buffers.contains(overflow.buffer));
        scheduler.retireStaging(0);
        scheduler.resetFrame(1);
        assert(shadow_fault::live_buffers.contains(overflow.buffer));
        scheduler.retireStaging(1);
        assert(shadow_fault::live_buffers.contains(overflow.buffer));
        scheduler.retireStaging(0);
        assert(!shadow_fault::live_buffers.contains(overflow.buffer));
    }
    {
        LifetimeCounts counts;
        Renderer renderer(*context);
        auto first = renderer.addScene({});
        assert(first);
        auto* accepted = renderer.getScene(first->scene_id);
        assert(accepted);
        assert(accepted->addFeature<LifetimeFeature>(counts));
        const auto view = accepted->addView({{16, 16}, "lifetime-view"});
        assert(accepted->getView(view) && counts.allocated == 1);
        shadow_fault::reject_scene_ring = true;
        const auto rejected = renderer.addScene({});
        shadow_fault::reject_scene_ring = false;
        assert(!rejected && rejected.error().args[0] == encodeVkResult(VK_ERROR_OUT_OF_DEVICE_MEMORY));
        assert(renderer.getScene(first->scene_id) == accepted);
        queue.flushAll();
        const auto second = renderer.addScene({});
        assert(second && second->scene_id.index == first->scene_id.index + 1);
        auto staging = accepted->transferScheduler().allocateStaging(256);
        assert(staging);
        std::memset(staging.mapped, 0x5a, 256);
        VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer_info.size = 256;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo allocation_info{};
        allocation_info.usage = VMA_MEMORY_USAGE_AUTO;
        allocation_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
        auto destination = VmaBuffer::create(device.vmaAllocator(), buffer_info, allocation_info);
        assert(destination);
        auto command = CommandBufferOwner::create(device.logicalDevice(), (*resources)->commandPool());
        assert(command);
        VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        assert(vkBeginCommandBuffer(command->get(), &begin_info) == VK_SUCCESS);
        const VkBufferCopy copy{staging.srcOffset, 0, 256};
        vkCmdCopyBuffer(command->get(), staging.buffer, destination->buffer(), 1, &copy);
        VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(command->get(), &dependency);
        assert(vkEndCommandBuffer(command->get()) == VK_SUCCESS);
        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        auto fence = FenceOwner::create(device.logicalDevice(), fence_info);
        assert(fence);
        const auto cmd = command->get();
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        assert(vkQueueSubmit(device.graphicsQueue(), 1, &submit, fence->get()) == VK_SUCCESS);
        renderer.setGpuCompletedSerial(40);
        renderer.removeScene(first->scene_id, 41);
        assert(!renderer.getScene(first->scene_id));
        renderer.collectRetiredScenes(100);
        assert(shadow_fault::live_buffers.contains(staging.buffer));
        assert(counts.released == 0 && counts.detached == 0 && counts.destroyed == 0);
        const auto completed_fence = fence->get();
        assert(vkWaitForFences(device.logicalDevice(), 1, &completed_fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS);
        renderer.setGpuCompletedSerial(41);
        renderer.collectRetiredScenes(100);
        assert(!shadow_fault::live_buffers.contains(staging.buffer));
        assert(counts.released == 1 && counts.detached == 1 && counts.destroyed == 1);
        const auto* mapped = static_cast<const unsigned char*>(destination->map());
        assert(mapped);
        assert(vmaInvalidateAllocation(device.vmaAllocator(), destination->allocation(), 0, 256) == VK_SUCCESS);
        for (unsigned i = 0; i != 256; ++i)
        {
            assert(mapped[i] == 0x5a);
        }
        destination->unmap();
        const auto replacement = renderer.addScene({});
        assert(replacement && replacement->scene_id != first->scene_id);
        assert(!renderer.getScene(first->scene_id));
    }
    queue.flushAll();
    assert(shadow_fault::live_buffers.size() == baseline && shadow_fault::live_pools.size() == baseline_pools);
    context->reset();
    assert(shadow_fault::live_buffers.empty());
    std::puts(
        "Context/Scene exact failure, no ID publication, retry, staging FIF and actual GPU watermark retirement PASS"
    );
}
