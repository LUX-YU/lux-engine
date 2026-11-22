#pragma once

#include "Support.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <Extensions/NRIWrapperD3D12.h>
#include <Extensions/NRIWrapperVK.h>
#include <vulkan/vulkan.h>
#undef interface

namespace foundation_test
{
    // A real timeline wait for native retirement tests. The submitted work cannot retire until the host signals it.
    struct RetirementFence final
    {
        explicit RetirementFence(DeviceSession& session) noexcept : core(session.nriCore())
        {
            auto& device = session.device();
            auto* native = core.GetDeviceNativeObject(&device);
            if (session.description().graphicsAPI == nri::GraphicsAPI::D3D12)
            {
                auto& dx_device = *static_cast<ID3D12Device*>(native);
                CHECK(SUCCEEDED(dx_device.CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&dx_fence))));
                nri::WrapperD3D12Interface wrapper{};
                CHECK(nri::nriGetInterface(device, NRI_INTERFACE(nri::WrapperD3D12Interface), &wrapper) ==
                    nri::Result::SUCCESS);
                CHECK(wrapper.CreateFenceD3D12(device, {dx_fence}, fence) == nri::Result::SUCCESS);
                return;
            }
            vk_device = static_cast<VkDevice>(native);
            nri::WrapperVKInterface wrapper{};
            CHECK(nri::nriGetInterface(device, NRI_INTERFACE(nri::WrapperVKInterface), &wrapper) == nri::Result::SUCCESS);
            const auto proc = reinterpret_cast<PFN_vkGetDeviceProcAddr>(wrapper.GetDeviceProcAddrVK(device));
            const auto create = reinterpret_cast<PFN_vkCreateSemaphore>(proc(vk_device, "vkCreateSemaphore"));
            destroy_vk = reinterpret_cast<PFN_vkDestroySemaphore>(proc(vk_device, "vkDestroySemaphore"));
            signal_vk = reinterpret_cast<PFN_vkSignalSemaphore>(proc(vk_device, "vkSignalSemaphore"));
            CHECK(create && destroy_vk && signal_vk);
            const VkSemaphoreTypeCreateInfo timeline{
                VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, nullptr, VK_SEMAPHORE_TYPE_TIMELINE, 0
            };
            const VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &timeline, 0};
            CHECK(create(vk_device, &info, nullptr, &vk_fence) == VK_SUCCESS);
            const nri::FenceVKDesc native_fence{reinterpret_cast<std::uintptr_t>(vk_fence)};
            CHECK(wrapper.CreateFenceVK(device, native_fence, fence) == nri::Result::SUCCESS);
        }

        RetirementFence(const RetirementFence&) = delete;
        RetirementFence& operator=(const RetirementFence&) = delete;

        ~RetirementFence() noexcept
        {
            core.DestroyFence(fence);
            if (dx_fence) dx_fence->Release();
            else destroy_vk(vk_device, vk_fence, nullptr);
        }

        void signal() const noexcept
        {
            if (dx_fence)
            {
                CHECK(SUCCEEDED(dx_fence->Signal(1)));
                return;
            }
            const VkSemaphoreSignalInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, nullptr, vk_fence, 1};
            CHECK(signal_vk(vk_device, &info) == VK_SUCCESS);
        }

        const nri::CoreInterface& core;
        nri::Fence* fence = nullptr;
        ID3D12Fence* dx_fence = nullptr;
        VkDevice vk_device = nullptr;
        VkSemaphore vk_fence = VK_NULL_HANDLE;
        PFN_vkDestroySemaphore destroy_vk = nullptr;
        PFN_vkSignalSemaphore signal_vk = nullptr;
    };
} // namespace foundation_test
