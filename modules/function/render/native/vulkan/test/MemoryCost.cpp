#include "../../../resource/test/FactoryAudit.hpp"
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
            std::printf("PreparedBinding=%zu StorageUse=%zu Registry=%zu ResourceDomain=%zu Factory=%zu\n",
                sizeof(PreparedStorageBinding), sizeof(ExecutionContext::StorageUse), sizeof(ResourceRegistry),
                sizeof(ResourceDomain), sizeof(StorageFactory));
            std::printf("SubmissionRequirements=%zu SubmissionCallback=%zu\n",
                sizeof(SubmissionRequirements), sizeof(SubmissionCallback));
            render_allocation::calls = 0;
            render_allocation::bytes = 0;
            render_allocation::window = true;
            {
                // CPU Registry metadata and one-byte dummy native owners only, explicitly NOT GPU allocations.
                ResourceRegistry registry(nullptr, 0);
                static const StorageDescriptor description{StorageNameView("test.cost")};
                static const unsigned ops = 0;
                const auto factory = resource_detail::StorageFactoryAudit::make(&description, &ops,
                    [](void*, const void*) noexcept -> RenderResult<NativeStorage> {
                        return NativeStorage{new std::byte{}, [](void* p) noexcept { delete static_cast<std::byte*>(p); }};
                    }, [](const void*, const void*) noexcept { return true; }, {}, nullptr, nullptr,
                    storageCppType<unsigned>(), storageCppType<unsigned>());
                if (!registry.installFactory(factory, 0u))
                    std::abort();
                for (unsigned i = 0; i < 1000; ++i)
                {
                    auto scene = registry.createDomain(registry.root(), EResourceScope::SCENE);
                    if (!scene || !registry.getOrCreate(*scene, "item", description, nullptr)) std::abort();
                }
                std::printf("Registry metadata 1000 domains + instances: cold_new=%zu cumulative_bytes=%zu\n",
                    render_allocation::calls.load(), render_allocation::bytes.load());
            }
            render_allocation::window = false;
        }
    };
}

int main() { lux::render::vulkan::NativeMemoryAudit::run(); }
