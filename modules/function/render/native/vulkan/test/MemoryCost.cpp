#include "../../../resource/test/FactoryAudit.hpp"
#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>
#include "Allocation.hpp"
#include "Support.hpp"
#include <chrono>
#include <fstream>
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
            std::printf("PreparedBinding=%zu StorageUse=%zu Registry=%zu StorageSet=%zu Factory=%zu\n",
                sizeof(PreparedStorageBinding), sizeof(ExecutionContext::StorageUse), sizeof(ResourceRegistry),
                sizeof(StorageSet), sizeof(StorageFactory));
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
                    [](void*, StorageId, const void*) noexcept -> RenderResult<NativeStorage> {
                        return NativeStorage{new std::byte{}, [](void* p) noexcept { delete static_cast<std::byte*>(p); }};
                    }, [](const void*, const void*) noexcept { return true; }, nullptr,
                    cppType<unsigned>(), cppType<unsigned>());
                if (!registry.installFactory(factory, 0u))
                    std::abort();
                std::vector<std::unique_ptr<StorageSet>> scenes;
                scenes.reserve(1000);
                for (unsigned i = 0; i < 1000; ++i)
                {
                    auto& scene = scenes.emplace_back(std::make_unique<StorageSet>(registry));
                    if (!scene->prepare(registry.typeId(description.type), "item")) std::abort();
                }
                std::printf("Registry metadata 1000 real owner sets + instances: cold_new=%zu cumulative_bytes=%zu\n",
                    render_allocation::calls.load(), render_allocation::bytes.load());
            }
            render_allocation::window = false;
        }

        static void zeroRetirement()
        {
            using namespace foundation_test;
            using Clock = std::chrono::steady_clock;
            Validation validation;
            auto bootstrap = instance(validation);
            auto session = take(DeviceSession::create(bootstrap, {}));
            struct LiveBuffer final
            {
                DeviceSession& session;
                RetirementReservation retirement;
                BufferPermit buffer;
                ~LiveBuffer() noexcept { CHECK(session.destroyUnsubmitted(buffer)); }
            };
            static const StorageDescriptor description{StorageNameView("test.cost.live-buffer")};
            static const unsigned ops = 0;
            auto factory = resource_detail::StorageFactoryAudit::make(&description, &ops,
                [](void* context, StorageId, const void*) noexcept -> RenderResult<NativeStorage> {
                    auto& session = *static_cast<DeviceSession*>(context);
                    auto reservation = session.retirement().reserve();
                    if (!reservation) return cxx::unexpected(reservation.error());
                    auto buffer = session.createBuffer(16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, EMemoryAccess::DEVICE);
                    if (!buffer) return cxx::unexpected(buffer.error());
                    return NativeStorage{new LiveBuffer{session, std::move(*reservation), std::move(*buffer)},
                        [](void* value) noexcept { delete static_cast<LiveBuffer*>(value); }};
                }, [](const void*, const void*) noexcept { return true; }, nullptr,
                cppType<unsigned>(), cppType<unsigned>());
            const auto type = take(session->resources().installFactory(std::move(factory), 0u));
            std::ofstream raw("zero-retirement.csv");
            raw << "live_buffers,live_storages,reservations,sample,collect_ns,candidate_visits,new_calls\n";
            std::vector<std::unique_ptr<StorageSet>> owners;
            for (const auto count : {1u, 100u, 1000u, 10000u})
            {
                while (owners.size() < count)
                {
                    auto& owner = owners.emplace_back(std::make_unique<StorageSet>(session->resources()));
                    CHECK(owner->prepare(type, "live"));
                }
                CHECK(session->buffers_.size() == count && session->resources().size() == count);
                CHECK(session->retirement().reservations() == count && session->retirement().pending() == 0);
                for (unsigned warm = 0; warm < 20; ++warm) CHECK(session->collect());
                for (unsigned sample = 0; sample < 1000; ++sample)
                {
                    const auto before = session->retirement().inspected_;
                    render_allocation::calls = 0;
                    render_allocation::window = true;
                    const auto start = Clock::now();
                    CHECK(session->collect());
                    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
                    render_allocation::window = false;
                    const auto visits = session->retirement().inspected_ - before;
                    CHECK(visits == 0 && render_allocation::calls == 0);
                    raw << count << ',' << count << ',' << count << ',' << sample << ',' << ns << ','
                        << visits << ',' << render_allocation::calls.load() << '\n';
                }
                std::printf("zero retirement: real VkBuffers/StorageSets/reservations=%u; 1000 polls, visits=0 new=0\n",
                    count);
            }
            owners.clear();
            CHECK(session->buffers_.empty() && session->resources().size() == 0);
            CHECK(session->retirement().reservations() == 0 && validation.errors == 0);
        }
    };
}

int main()
{
    lux::render::vulkan::NativeMemoryAudit::run();
    lux::render::vulkan::NativeMemoryAudit::zeroRetirement();
}
