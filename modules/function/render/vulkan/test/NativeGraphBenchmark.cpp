#include "Allocation.hpp"
#include "F3Tonemap.pass.hpp"
#include "F4Fill.pass.hpp"
#include "F4Local.pass.hpp"
#include "F4Readback.pass.hpp"
#include "NativeGraphSupport.hpp"

using namespace native_graph_test;
using Clock = std::chrono::steady_clock;

namespace
{
    template <std::size_t N>
    void report(
        const char* name,
        std::array<double, N> raw,
        std::uint64_t count,
        std::uint64_t bytes,
        bool census = true
    )
    {
        auto sorted = raw;
        std::sort(sorted.begin(), sorted.end());
        std::printf(
            "{\"case\":\"%s\",\"p50_ns\":%.3f,\"p95_ns\":%.3f,\"max_ns\":%.3f,"
            "\"allocations\":%llu,\"allocation_bytes\":%llu,\"allocation_census\":%s,\"samples_ns\":[",
            name,
            sorted[N / 2],
            sorted[N * 95 / 100],
            sorted.back(),
            count,
            bytes,
            census ? "true" : "false"
        );
        for (std::size_t i = 0; i < N; ++i)
        {
            std::printf("%s%.3f", i ? "," : "", raw[i]);
        }
        std::puts("]}");
    }

    // Measurement-only native query pool, never a second production resource owner.
    struct TimestampQueries
    {
        VkDevice device;
        VkQueryPool pool;

        explicit TimestampQueries(VkDevice device) : device(device)
        {
            VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            info.queryType = VK_QUERY_TYPE_TIMESTAMP;
            info.queryCount = 2;
            CHECK(vkCreateQueryPool(device, &info, nullptr, &pool) == VK_SUCCESS);
        }

        ~TimestampQueries()
        {
            vkDestroyQueryPool(device, pool, nullptr);
        }

        TimestampQueries(const TimestampQueries&) = delete;
    };
} // namespace

int main(int argc, char** argv)
{
    CHECK(argc == 2);
    const std::filesystem::path shaders(argv[1]);
    constexpr bool local_read = false, multiple = false;
    auto host = checked(VulkanInstance::create());
    DeviceOptions options;
    options.dynamic_rendering = true;
    auto device = checked(VulkanDevice::create(host, options));
    auto allocator = checked(VulkanAllocator::create(device));
    auto queue = checked(SubmissionQueue::create(device, 16));
    auto* compute_queue = queue.get();
    auto* transfer_queue = queue.get();
    auto sampler = checked(Sampler::create(device));
    RenderGraphBuilder builder;
    TextureDesc texture;
    texture.width = texture.height = 256;
    const auto hdr = builder.texture(texture, "hdr");
    const auto ldr = builder.texture(texture, "ldr");
    const auto output = builder.buffer({256 * 256 * 4 * sizeof(float), 4}, "readback");
    F4Fill fill;
    fill.output.texture = hdr;
    const auto compute = checked(builder.compute("fill", {{"F4Fill", "default"}}, EExecutionScope::VIEW, fill));
    F3Tonemap tonemap;
    tonemap.input.texture = hdr;
    tonemap.nearest.sampler = GraphSampler{1};
    tonemap.output.texture = ldr;
    F4Local local;
    local.input.texture = hdr;
    local.output.texture = ldr;
    const auto graphics =
        local_read ? checked(builder.graphics("tonemap", {{"F4Local", "default"}}, EExecutionScope::VIEW, local))
                   : checked(builder.graphics("tonemap", {{"F3Tonemap", "default"}}, EExecutionScope::VIEW, tonemap));
    F4Readback readback;
    readback.source.texture = ldr;
    readback.destination.buffer = output;
    const auto transfer = checked(builder.transfer("readback", EExecutionScope::VIEW, readback));
    CHECK(builder.exportBuffer(output, PassProducer{passKey("readback")}, EGraphOutput::READBACK));
    auto definition = checked(std::move(builder).finish());
    auto logical = checked(compileLogicalGraph(definition));
    auto invocation = makeGraphInvocationData(definition);
    std::vector<NativeShaderProgram> programs;
    programs.push_back(program(device, logical, compute, "F4Fill", PassSchema<F4Fill>::contract(), shaders, true));
    GraphicsDescription local_graphics;
    local_graphics.color_formats = {VK_FORMAT_R32G32B32A32_SFLOAT};
    local_graphics.blends.resize(1);
    local_graphics.color_input_indices = {VK_ATTACHMENT_UNUSED};
    programs.push_back(
        local_read ? program(
                         device,
                         logical,
                         graphics,
                         "F4Local",
                         PassSchema<F4Local>::contract(),
                         shaders,
                         false,
                         0,
                         &local_graphics
                     )
                   : program(device, logical, graphics, "F3Tonemap", PassSchema<F3Tonemap>::contract(), shaders, false)
    );
    const std::array commands{
        NativePassCommand{compute, DispatchCommand{256, 256, 1}, multiple ? EQueueRole::COMPUTE : EQueueRole::GRAPHICS},
        NativePassCommand{graphics, DrawCommand{}, EQueueRole::GRAPHICS},
        NativePassCommand{transfer, CopyCommand{}, multiple ? EQueueRole::TRANSFER : EQueueRole::GRAPHICS}
    };
    const std::array samplers{std::pair{GraphSampler{1}, std::cref(sampler)}};
    NativeCompileInputs inputs{
        logical,
        device,
        allocator,
        {queue.get(), compute_queue, transfer_queue},
        commands,
        {},
        invocation,
        samplers
    };

    auto executable = checked(compileVulkanGraph(inputs, std::move(programs)));
    std::array<GraphImportBinding, 0> imports;
    std::array<double, 100> cpu{}, gpu{};
    TimestampQueries queries(device.native());
    std::uint64_t allocations = 0, bytes = 0;
    for (unsigned serial = 0; serial < 110; ++serial)
    {
        auto start_batch = checked(queue->begin());
        vkCmdResetQueryPool(start_batch.native(), queries.pool, 0, 2);
        vkCmdWriteTimestamp2(start_batch.native(), VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, queries.pool, 0);
        CHECK(std::move(start_batch).submit());
        auto frame =
            checked(FrameGraphBindings::create(executable.logical(), {serial, serial, serial % 2}, imports, invocation)
            );
        allocation_count = allocation_bytes = 0;
        measuring = serial >= 10;
        const auto start = Clock::now();
        auto receipt = checked(executable.submit(frame, {}));
        const auto ns = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
        measuring = false;
        auto end_batch = checked(queue->begin());
        vkCmdWriteTimestamp2(end_batch.native(), VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, queries.pool, 1);
        auto end = checked(std::move(end_batch).submit());
        CHECK(checked(queue->wait(end, UINT64_MAX)));
        std::array<std::uint64_t, 2> ticks;
        CHECK(
            vkGetQueryPoolResults(
                device.native(),
                queries.pool,
                0,
                2,
                sizeof(ticks),
                ticks.data(),
                sizeof(std::uint64_t),
                VK_QUERY_RESULT_64_BIT
            ) == VK_SUCCESS
        );
        if (serial >= 10)
        {
            cpu[serial - 10] = ns;
            gpu[serial - 10] = double(ticks[1] - ticks[0]) * device.properties().limits.timestampPeriod;
            allocations += allocation_count.load();
            bytes += allocation_bytes.load();
        }
        if (serial == 109)
        {
            std::vector<float> pixels(256 * 256 * 4);
            CHECK(executable
                      .readback(receipt, GraphResourceId{output.value()}, 0, std::as_writable_bytes(std::span{pixels}))
            );
            for (unsigned y = 0; y < 256; ++y)
            {
                for (unsigned x = 0; x < 256; ++x)
                {
                    CHECK(std::abs(pixels[(y * 256 + x) * 4] - float(x + 1) / float(x + 2)) < 1e-6f);
                }
            }
        }
    }
    report("native_graph_record_submit_256x256", cpu, allocations, bytes);
    report("gpu_queue_interval_including_submission_gaps", gpu, 0, 0, false);
    CHECK(allocations == 0 && bytes == 0);
    std::array<std::array<double, 30>, 5> cold{};
    allocations = bytes = 0;
    for (unsigned sample = 0; sample < 30; ++sample)
    {
        std::vector<NativeShaderProgram> candidates;
        candidates.push_back(program(device, logical, compute, "F4Fill", PassSchema<F4Fill>::contract(), shaders, true)
        );
        candidates.push_back(
            program(device, logical, graphics, "F3Tonemap", PassSchema<F3Tonemap>::contract(), shaders, false)
        );
        allocation_count = allocation_bytes = 0;
        measuring = true;
        const auto start = Clock::now();
        auto candidate = checked(compileVulkanGraph(inputs, std::move(candidates)));
        cold[4][sample] = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
        measuring = false;
        allocations += allocation_count.load();
        bytes += allocation_bytes.load();
        const auto statistics = candidate.statistics();
        for (unsigned phase = 0; phase < 4; ++phase)
        {
            cold[phase][sample] = double(statistics.compile_ns[phase]);
        }
    }
    report("C9_identity_and_candidate", cold[0], 0, 0, false);
    report("C10_native_resources", cold[1], 0, 0, false);
    report("C11_fixed_recipes_and_descriptors", cold[2], 0, 0, false);
    report("C12_C13_sync_queue_admission", cold[3], 0, 0, false);
    report("native_graph_compile_prebuilt_programs", cold[4], allocations, bytes);
    std::printf(
        "BENCHMARK device=%s driver=%u API=%u timestamp_period=%.9f FIF=2 width=256 height=256 "
        "validation=OFF warmup=10 record_samples=100 cold_samples=30 lazy_shader_PSO_layout=0\n",
        device.properties().deviceName,
        device.properties().driverVersion,
        device.properties().apiVersion,
        device.properties().limits.timestampPeriod
    );
}
