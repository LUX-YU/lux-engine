#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>
#include "Allocation.hpp"
#include <cstdio>
#include <cstdlib>

namespace lux::render::vulkan
{
    struct NativeMemoryAudit final
    {
        static void run()
        {
            constexpr std::size_t count = 10000;
            render_allocation::participate = true;
            render_allocation::calls = 0;
            render_allocation::window = true;
            // Host metadata only: no fake allocation is published to a DeviceSession or GPU.
            cxx::StableSlotMap<BufferAllocation, BufferAllocationTag> buffers;
            cxx::StableSlotMap<ImageAllocation, ImageAllocationTag> images;
            buffers.reserve(count / 2);
            images.reserve(count / 2);
            std::vector<ExecutionContext::Binding> bindings(count);
            for (std::size_t i = 0; i < count / 2; ++i)
            {
                const auto b = buffers.emplace(VK_NULL_HANDLE, nullptr, nullptr, 16,
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT, EMemoryAccess::DEVICE);
                bindings[i].buffer = buffers.find(b);
                const auto image = images.emplace(VK_NULL_HANDLE, nullptr,
                    ImageDescription{{16, 16}, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT});
                bindings[i + count / 2].image = images.find(image);
            }
            render_allocation::window = false;
            const auto bytes = buffers.storageBytes() + images.storageBytes() + sizeof(bindings) +
                bindings.capacity() * sizeof(ExecutionContext::Binding);
            std::printf("VkBuffer=%zu VkImage=%zu BufferAllocation=%zu ImageAllocation=%zu Binding=%zu "
                "Context=%zu RetirementEntry=%zu host_records=%zu ledger_and_bindings_bytes=%zu new_calls=%llu\n",
                sizeof(VkBuffer), sizeof(VkImage), sizeof(BufferAllocation), sizeof(ImageAllocation),
                sizeof(ExecutionContext::Binding), sizeof(ExecutionContext), sizeof(RetirementQueue::Entry),
                count, bytes, render_allocation::calls.load());
        }
    };
}

int main() { lux::render::vulkan::NativeMemoryAudit::run(); }
