#include <cassert>
#include <chrono>
#include <cstdio>
#include <lux/engine/render/gpu/VulkanContext.hpp>
#include <lux/engine/render/gpu/descriptor/DescriptorService.hpp>
#include <lux/engine/render/gpu/descriptor/SceneDescriptorArena.hpp>
#include <lux/engine/render/gpu/lifecycle/DeferredDestroyQueue.hpp>
#include <lux/engine/render/gpu/pipeline/GeneralDescriptorSetLayout.hpp>
#include <lux/engine/render/resources/lighting/LightResources.hpp>
#include <lux/engine/render/resources/mesh/InstanceResources.hpp>
#include <vector>

int main()
{
    using namespace lux::render;
    using Clock = std::chrono::steady_clock;
    auto instance_owner = InstanceContext::create({});
    assert(instance_owner);
    auto& instance = **instance_owner;
    DeviceContext device(instance);
    assert(device.init(EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED));
    DeferredDestroyQueue retire(device);
    DescriptorService descriptors(device.logicalDevice());
    auto layouts = GeneralDescriptorSetLayout::create(device);
    auto arena = SceneDescriptorArena::create(device.logicalDevice(), {});
    assert(layouts && arena);
    const auto allocated = (*arena)->allocate((*layouts)->getLayout(EDescriptorSetSlot::INSTANCE));
    assert(allocated);
    const std::array targets{*allocated};
    for (const std::size_t count : {256U, 4096U})
    {
        for (const bool sparse : {false, true})
        {
            InstanceResources::CreateInfo info{device, retire, targets};
            info.initial_capacity = info.max_capacity = 8192;
            auto owner = InstanceResources::create(info);
            assert(owner);
            auto& resources = **owner;
            const auto anonymous = resources.allocateObject();
            assert(anonymous && resources.isAlive(anonymous));
            assert(!resources.findSource(ERenderEntityId{}));
            resources.freeObject(anonymous);
            assert(!resources.isAlive(anonymous) && resources.aliveCount() == 0);

            std::vector<RenderObjectHandle> handles;
            handles.reserve(count);
            const auto source = [sparse](std::size_t i)
            { return static_cast<ERenderEntityId>(sparse ? (1ULL << 50) + i * 1048576ULL : i); };
            const auto begin = Clock::now();
            for (std::size_t i = 0; i < count; ++i)
            {
                const auto handle = resources.allocateObject();
                assert(handle);
                handles.push_back(handle);
                assert(resources.bindSource(source(i), handle) == InstanceResources::ESourceBindResult::INSERTED);
                assert(resources.bindSource(source(i), handle) == InstanceResources::ESourceBindResult::ALREADY_BOUND);
            }
            const auto created = Clock::now();
            std::uint64_t checksum{};
            for (std::size_t iteration = 0; iteration < 100; ++iteration)
            {
                for (std::size_t i = 0; i < count; ++i)
                {
                    const auto found = resources.findSource(source(i));
                    assert(found == handles[i]);
                    checksum += found.index;
                }
            }
            const auto looked_up = Clock::now();
            for (std::size_t i = 0; i < count; ++i)
            {
                resources.freeObject(handles[i]);
                assert(!resources.findSource(source(i)));
            }
            const auto retired = Clock::now();
            const auto newer = resources.allocateObject();
            assert(newer && newer != handles.back());
            assert(resources.bindSource(source(0), newer) == InstanceResources::ESourceBindResult::INSERTED);
            resources.freeObject(handles.back());
            assert(resources.findSource(source(0)) == newer && resources.aliveCount() == 1);
            resources.freeObject(newer);
            assert(!resources.findSource(source(0)) && resources.aliveCount() == 0);
            owner->reset();
            // No GPU commands were submitted in this owner test. Integration tests separately
            // establish real View CPU-reference / GPU-completion retirement.
            retire.collect(0);
            assert(retire.pendingCount() == 0);
            std::printf(
                "owner count=%zu sparse=%d lookups=%zu checksum=%llu create_us=%.3f lookup_us=%.3f retire_us=%.3f "
                "logical_index_payload=%zu anonymous=1 final_alive=0 pending_gpu_frees=0 submissions=0\n",
                count,
                sparse,
                count * 100,
                static_cast<unsigned long long>(checksum),
                std::chrono::duration<double, std::micro>(created - begin).count(),
                std::chrono::duration<double, std::micro>(looked_up - created).count(),
                std::chrono::duration<double, std::micro>(retired - looked_up).count(),
                count * 32
            );
        }
    }
    {
        std::array<VkDescriptorSetLayoutBinding, 5> bindings{};
        for (std::uint32_t i = 0; i < 4; ++i)
        {
            bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        }
        bindings[4] = {
            static_cast<std::uint32_t>(ELightSetBindings::SHADING_INPUTS),
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            kShadingInputSlotCount,
            VK_SHADER_STAGE_FRAGMENT_BIT,
            nullptr
        };
        const auto layout = descriptors.registerLayout({.bindings = bindings, .debug_name = "owner light check"});
        assert(layout);
        const auto set = (*arena)->allocate(descriptors.layout(*layout));
        assert(set);
        LightResources::CreateInfo info{};
        info.ssbo_config = SSBOInitConfig{&device, &retire, 16, 1};
        info.descriptor_svc = &descriptors;
        info.domain_sets = std::span(&*set, 1);
        auto owner = LightResources::create(info);
        assert(owner);
        auto& lights = **owner;
        const auto anonymous = lights.submit(PointLightDesc{});
        assert(anonymous && lights.findSource(ERenderEntityId{}).isNull());
        lights.remove(*anonymous);
        assert(lights.lightCount(ELightSetBindings::LIGHT_POINT) == 0);
        const auto next = lights.submit(PointLightDesc{});
        assert(next && *next != *anonymous);
        assert(lights.bindSource(ERenderEntityId{}, *next) == LightResources::ESourceBindResult::INSERTED);
        lights.remove(*anonymous);
        assert(lights.findSource(ERenderEntityId{}) == *next && lights.lightCount(ELightSetBindings::LIGHT_POINT) == 1);
        lights.remove(*next);
        assert(lights.findSource(ERenderEntityId{}).isNull() && lights.lightCount(ELightSetBindings::LIGHT_POINT) == 0);
        owner->reset();
    }
    retire.collect(0);
    assert(retire.pendingCount() == 0);
    std::puts("PASS R06/R07 Mesh and Light owners: anonymous allocation/free, complete source keys, stale handles, "
              "owner release and empty indices; real GPU allocations, no submitted work");
}
