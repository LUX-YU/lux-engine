#include "Allocation.hpp"
#include "SharedFixture.hpp"
#include <lux/engine/toolchain/shader/SpirvRelocation.hpp>

using Clock = std::chrono::steady_clock;

static double elapsed(Clock::time_point start)
{
    return std::chrono::duration<double, std::nano>(Clock::now() - start).count();
}

template <std::size_t N>
static void report(const char* name, std::array<double, N> samples, unsigned batch = 1, bool allocation_census = true)
{
    const auto raw_samples = samples;
    std::sort(samples.begin(), samples.end());
    std::printf(
        "{\"case\":\"%s\",\"operations\":%zu,\"batch\":%u,\"p50_ns\":%.3f,\"p95_ns\":%.3f,\"max_ns\":%.3f,"
        "\"allocation_census\":%s,\"allocations\":%llu,\"allocation_bytes\":%llu,\"samples_ns\":[",
        name,
        N * batch,
        batch,
        samples[N / 2],
        samples[N * 95 / 100],
        samples.back(),
        allocation_census ? "true" : "false",
        static_cast<unsigned long long>(allocation_count.load()),
        static_cast<unsigned long long>(allocation_bytes.load())
    );
    for (std::size_t i = 0; i < raw_samples.size(); ++i)
    {
        std::printf("%s%.3f", i == 0 ? "" : ",", raw_samples[i]);
    }
    std::puts("]}");
}

template <class F> static void cold(const char* name, F&& operation)
{
    for (unsigned i = 0; i < 5; ++i)
    {
        operation();
    }
    std::array<double, 100> samples;
    allocation_count = allocation_bytes = 0;
    measuring = true;
    for (auto& sample : samples)
    {
        const auto start = Clock::now();
        operation();
        sample = elapsed(start);
    }
    measuring = false;
    report(name, samples);
}

int main(int argc, char** argv)
{
    CHECK(argc == 4);
    const std::filesystem::path binaries(argv[1]), sources(argv[2]), evidence(argv[3]);
    std::filesystem::create_directories(evidence);
    // Performance run intentionally has no validation instrumentation; correctness uses the separate G01-G07 runs.
    auto host = take(VulkanInstance::create());
    DeviceOptions options;
    options.dynamic_rendering = true;
    auto device = take(VulkanDevice::create(host, options));
    auto allocator = take(VulkanAllocator::create(device));
    auto queue = take(SubmissionQueue::create(device, 3));
    const auto schema = PassSchema<F3SharedA>::contract();
    const std::array schemas{schema, PassSchema<F3SharedB>::contract()};
    const std::array owners{
        take(declareOwnerShape("a.scene", 1, lux::rdesc::EFieldOwner::SCENE, schemas)),
        take(declareOwnerShape("b.feature", 1, lux::rdesc::EFieldOwner::FEATURE, schemas))
    };
    const OwnerAssignment assignment{owners[0].identity, owners[1].identity, {}};
    const auto caps = queryLayoutCaps(device);
    const auto layout = take(compileLayout(schema, assignment, owners, caps));
    std::vector<PassDescriptorLocation> locations;
    for (const auto& field : layout.identity().fields)
    {
        locations.push_back({field.field_index, field.set, field.binding});
    }
    const auto vertex = words(binaries / "F3SharedA.vert.spv");
    cold("layout_full_shape", [&] { CHECK(compileLayout(schema, assignment, owners, caps)); });
    cold("relocation_validation_vertex", [&] { CHECK(relocatePassSpirv(vertex, schema, locations, 1)); });
    cold(
        "complete_native_candidate_including_cook_io",
        [&] { CHECK(program<F3SharedA>(device, binaries, sources, owners)); }
    );
    auto active = take(program<F3SharedA>(device, binaries, sources, owners));
    std::vector<VkDescriptorSetLayout> native_sets;
    for (const auto& set : active.setLayouts())
    {
        native_sets.push_back(set.native());
    }
    auto pipeline_layout = take(PipelineLayout::create(device, native_sets));
    auto vertex_module = take(ShaderModule::create(device, active.identity().relocated[0].words));
    auto fragment_module = take(ShaderModule::create(device, active.identity().relocated[1].words));
    cold(
        "native_graphics_pipeline_create_destroy",
        [&]
        {
            CHECK(GraphicsPipeline::create(
                device,
                vertex_module,
                fragment_module,
                pipeline_layout,
                std::get<GraphicsDescription>(active.identity().pipeline)
            ));
        }
    );
    auto buffer = take(Buffer::create(allocator, 16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, EMemoryAccess::UPLOAD));
    const F3SharedValue data{1, 2, 3, 4};
    CHECK(buffer.write(0, std::as_bytes(std::span(&data, 1))));
    std::vector<OwnerDescriptorValue> values;
    for (const auto& owner : owners)
    {
        for (const auto& field : owner.fields)
        {
            values.push_back({owner.identity, field.semantic, 0, BufferDescriptorValue{std::cref(buffer), 0, 16}});
        }
    }
    cold("complete_descriptor_pool_sets", [&] { CHECK(BoundDescriptorSets::create(device, active, values)); });
    auto descriptors = take(BoundDescriptorSets::create(device, active, values));
    const std::vector<std::byte> scalars(active.identity().graph.passes[0].scalar_size);
    const auto batch = [&](bool measure)
    {
        auto command = take(queue->begin());
        measuring = measure;
        const auto start = Clock::now();
        for (unsigned i = 0; i < 1000; ++i)
        {
            CHECK(descriptors.bind(command.native(), {}, scalars));
        }
        const double cost = elapsed(start) / 1000;
        measuring = false;
        const auto ticket = take(std::move(command).submit());
        CHECK(take(queue->wait(ticket, 10'000'000'000ull)));
        return cost;
    };
    for (unsigned i = 0; i < 10; ++i)
    {
        batch(false);
    }
    std::array<double, 1000> hot;
    allocation_count = allocation_bytes = 0;
    for (auto& sample : hot)
    {
        sample = batch(true);
    }
    CHECK(allocation_count == 0 && allocation_bytes == 0);
    report("fixed_native_binding_recipe", hot, 1000);

    std::uint32_t family_count{};
    vkGetPhysicalDeviceQueueFamilyProperties(device.physical(), &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(device.physical(), &family_count, families.data());
    const auto bits = families[device.queueFamily()].timestampValidBits;
    CHECK(bits > 0);

    struct TimestampPool
    {
        VkDevice device;
        VkQueryPool handle;

        ~TimestampPool()
        {
            vkDestroyQueryPool(device, handle, nullptr);
        }
    };

    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = 2;
    VkQueryPool native_pool{};
    CHECK(vkCreateQueryPool(device.native(), &info, nullptr, &native_pool) == VK_SUCCESS);
    TimestampPool timestamps{device.native(), native_pool};
    const auto gpu_sample = [&]
    {
        auto pending = submitSharedDraw(allocator, *queue, active, descriptors, timestamps.handle);
        CHECK(take(queue->wait(pending.ticket, 10'000'000'000ull)));
        std::array<std::uint64_t, 2> result{};
        CHECK(
            vkGetQueryPoolResults(
                device.native(),
                timestamps.handle,
                0,
                2,
                sizeof(result),
                result.data(),
                sizeof(result[0]),
                VK_QUERY_RESULT_64_BIT
            ) == VK_SUCCESS
        );
        const auto mask = bits == 64 ? ~std::uint64_t{} : (std::uint64_t{1} << bits) - 1;
        return double((result[1] - result[0]) & mask) * device.properties().limits.timestampPeriod;
    };
    for (unsigned i = 0; i < 10; ++i)
    {
        gpu_sample();
    }
    std::array<double, 100> gpu;
    allocation_count = allocation_bytes = 0;
    for (auto& sample : gpu)
    {
        sample = gpu_sample();
    }
    report("gpu_shared_2x2_draw_barriers_readback_timestamp", gpu, 1, false);
    std::printf(
        "DEVICE %s driver=%u api=%u timestamp_bits=%u period=%g; new workload baseline, no V1 comparison\n",
        device.properties().deviceName,
        device.properties().driverVersion,
        device.properties().apiVersion,
        bits,
        device.properties().limits.timestampPeriod
    );
}
