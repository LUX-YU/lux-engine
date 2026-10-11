#include "F3Tonemap.pass.hpp"
#include "F4Fill.pass.hpp"
#include "F4Readback.pass.hpp"
#include "F4Views.pass.hpp"
#include "NativeGraphSupport.hpp"

namespace native_graph_test
{
    void conditionalCase(const std::filesystem::path& shaders, Validation& validation)
    {
        auto host = instance(validation);
        DeviceOptions options;
        options.dynamic_rendering = true;
        options.multiple_queues = true;
        auto device = checked(VulkanDevice::create(host, options));
        auto allocator = checked(VulkanAllocator::create(device));
        auto queue = checked(SubmissionQueue::create(device, 32));
        auto compute_queue = checked(SubmissionQueue::create(device, 32, EQueueRole::COMPUTE));
        auto transfer_queue = checked(SubmissionQueue::create(device, 32, EQueueRole::TRANSFER));
        auto sampler = checked(Sampler::create(device));
        RenderGraphBuilder builder;
        TextureDesc desc;
        desc.width = desc.height = 4;
        F4Fill fill;
        fill.output.texture = builder.texture(desc, "conditional.source");
        auto producer = checked(builder.compute("optional", {{"F4Fill", "default"}}, EExecutionScope::VIEW, fill));
        CHECK(builder.condition(passKey("optional"), graphResourceKey("enabled")));
        F4Views fallback;
        fallback.output.texture = builder.texture(desc, "fallback.source");
        auto alternative =
            checked(builder.graphics("alternative", {{"F4Views", "default"}}, EExecutionScope::VIEW, fallback));
        F3Tonemap consumer;
        consumer.input.texture = fill.output.texture;
        consumer.input.fallback = fallback.output.texture;
        consumer.nearest.sampler = GraphSampler{1};
        consumer.output.texture = builder.texture(desc, "conditional.result");
        auto tone = checked(builder.graphics("tone", {{"F3Tonemap", "default"}}, EExecutionScope::VIEW, consumer));
        CHECK(builder.source(
            passKey("tone"),
            "input",
            PassProducer{passKey("optional")},
            AuthoringFallback{fallback.output.texture, PassProducer{passKey("alternative")}}
        ));
        F4Readback read;
        read.source.texture = consumer.output.texture;
        read.destination.buffer = builder.buffer({256, 4}, "conditional.bytes");
        auto copy = checked(builder.transfer("read", EExecutionScope::VIEW, read));
        CHECK(builder.exportBuffer(read.destination.buffer, PassProducer{passKey("read")}, EGraphOutput::READBACK));
        auto definition = checked(std::move(builder).finish());
        auto logical = checked(compileLogicalGraph(definition));
        auto invocation = makeGraphInvocationData(definition);
        std::vector<NativeShaderProgram> programs;
        programs.push_back(program(device, logical, producer, "F4Fill", PassSchema<F4Fill>::contract(), shaders, true));
        programs.push_back(
            program(device, logical, alternative, "F4Views", PassSchema<F4Views>::contract(), shaders, false)
        );
        programs.push_back(
            program(device, logical, tone, "F3Tonemap", PassSchema<F3Tonemap>::contract(), shaders, false)
        );
        const std::array commands{
            NativePassCommand{producer, DispatchCommand{4, 4, 1}, EQueueRole::COMPUTE},
            NativePassCommand{alternative, DrawCommand{}, EQueueRole::GRAPHICS},
            NativePassCommand{tone, DrawCommand{}, EQueueRole::GRAPHICS},
            NativePassCommand{copy, CopyCommand{}, EQueueRole::TRANSFER}
        };
        const std::array samplers{std::pair{GraphSampler{1}, std::cref(sampler)}};
        NativeCompileInputs inputs{
            logical,
            device,
            allocator,
            {queue.get(), compute_queue.get(), transfer_queue.get()},
            commands,
            {},
            invocation,
            samplers
        };
        auto executable = checked(compileVulkanGraph(inputs, std::move(programs)));
        std::array<GraphImportBinding, 0> imports;
        std::puts(executable.diagnostics().c_str());
        std::vector<NativeTraceEvent> trace(executable.traceCapacity());
        for (unsigned serial = 0; serial < 8; ++serial)
        {
            // Each slot observes both choices, including after a previously enabled producer.
            const bool enabled = (serial / 2) % 2 == 0;
            invocation.passes[producer.value() - 1].enabled = enabled;
            std::printf("CONDITIONAL serial=%u enabled=%u\n", serial, unsigned(enabled));
            auto frame = checked(
                FrameGraphBindings::create(executable.logical(), {serial, serial, serial % 2}, imports, invocation)
            );
            auto receipt = checked(executable.submit(frame, {}, trace));
            const auto observed = std::span{trace}.first(receipt.trace_count);
            const auto skips = std::count_if(
                observed.begin(),
                observed.end(),
                [](const auto& event) { return event.operation == ENativeTrace::SKIP; }
            );
            CHECK(skips == (enabled ? 0 : 1));
            if (serial == 0 || serial == 2)
            {
                std::printf("NATIVE_TRACE %s\n", nativeTraceJson(observed).c_str());
            }
            CHECK(checked(queue->wait(receipt.completion, UINT64_MAX)));
            std::array<float, 64> pixels;
            CHECK(executable.readback(
                receipt,
                GraphResourceId{read.destination.buffer.value()},
                0,
                std::as_writable_bytes(std::span{pixels})
            ));
            for (unsigned y = 0; y < 4; ++y)
            {
                for (unsigned x = 0; x < 4; ++x)
                {
                    const auto expected = enabled ? float(x + 1) / float(x + 2) : 0.2f;
                    CHECK(std::abs(pixels[(y * 4 + x) * 4] - expected) < 1e-6f);
                }
            }
        }
        std::puts("CONDITIONAL fallback_descriptor_and_barrier=PASS three_queues=1 frames=8");
    }
} // namespace native_graph_test
