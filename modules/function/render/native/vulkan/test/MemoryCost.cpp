#include "../../../resource/test/FactoryAudit.hpp"
#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>
#include "Allocation.hpp"
#include "Support.hpp"
#include <chrono>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace
{
    unsigned live_buffers = 0;
    std::uint64_t fence_queries = 0;
}

namespace lux::render::vulkan::test
{
    VkResult before(const char* operation) noexcept
    {
        if (std::string_view(operation) == "fence_status") ++fence_queries;
        return VK_SUCCESS;
    }

    VkResult after(const char* operation, VkResult result) noexcept
    {
        if (result == VK_SUCCESS && std::string_view(operation) == "buffer") ++live_buffers;
        return result;
    }

    void destroyed(const char* operation) noexcept
    {
        if (std::string_view(operation) == "buffer") --live_buffers;
    }
}

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
            // Actual binding storage only. Session no longer allocates a Buffer/Image identity ledger.
            std::vector<ExecutionContext::Binding> bindings(count);
            render_allocation::window = false;
            const auto bytes = sizeof(bindings) + bindings.capacity() * sizeof(ExecutionContext::Binding);
            std::printf("VkBuffer=%zu VkImage=%zu Buffer=%zu Image=%zu Binding=%zu "
                "Context=%zu binding_records=%zu bindings_bytes=%zu new_calls=%llu\n",
                sizeof(VkBuffer), sizeof(VkImage), sizeof(Buffer), sizeof(Image),
                sizeof(ExecutionContext::Binding), sizeof(ExecutionContext),
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
                std::shared_ptr<Buffer> buffer;
            };
            static const StorageDescriptor description{StorageNameView("test.cost.live-buffer")};
            static const unsigned ops = 0;
            auto factory = resource_detail::StorageFactoryAudit::make(&description, &ops,
                [](void* context, StorageId, const void*) noexcept -> RenderResult<NativeStorage> {
                    auto& session = *static_cast<DeviceSession*>(context);
                    auto buffer = session.allocator().createBuffer(16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, EMemoryAccess::DEVICE);
                    if (!buffer) return cxx::unexpected(buffer.error());
                    return NativeStorage{new LiveBuffer{std::move(*buffer)},
                        [](void* value) noexcept { delete static_cast<LiveBuffer*>(value); }};
                }, [](const void*, const void*) noexcept { return true; }, nullptr,
                cppType<unsigned>(), cppType<unsigned>());
            const auto type = take(session->resources().installFactory(std::move(factory), 0u));
            std::ofstream raw("zero-retirement.csv");
            raw << "live_buffers,live_storages,sample,collect_ns,fence_queries,new_calls\n";
            std::vector<std::unique_ptr<StorageSet>> owners;
            for (const auto count : {1u, 100u, 1000u, 10000u})
            {
                while (owners.size() < count)
                {
                    auto& owner = owners.emplace_back(std::make_unique<StorageSet>(session->resources()));
                    CHECK(owner->prepare(type, "live"));
                }
                CHECK(live_buffers == count && session->resources().size() == count);
                std::printf("real_live_buffers=%u sizeof_Buffer=%zu; Session allocation ledger removed\n", count, sizeof(Buffer));
                for (unsigned warm = 0; warm < 20; ++warm) CHECK(session->collect());
                for (unsigned sample = 0; sample < 1000; ++sample)
                {
                    const auto before = fence_queries;
                    render_allocation::calls = 0;
                    render_allocation::window = true;
                    const auto start = Clock::now();
                    CHECK(session->collect());
                    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
                    render_allocation::window = false;
                    const auto visits = fence_queries - before;
                    CHECK(visits == 0 && render_allocation::calls == 0);
                    raw << count << ',' << count << ',' << sample << ',' << ns << ','
                        << visits << ',' << render_allocation::calls.load() << '\n';
                }
                std::printf("zero retirement: real VkBuffers/StorageSets=%u; 1000 polls, native fence queries=0 new=0\n",
                    count);
            }
            owners.clear();
            CHECK(live_buffers == 0 && session->resources().size() == 0);
            CHECK(validation.errors == 0);
        }
    };
}

int main()
{
    lux::render::vulkan::NativeMemoryAudit::run();
    lux::render::vulkan::NativeMemoryAudit::zeroRetirement();
}
