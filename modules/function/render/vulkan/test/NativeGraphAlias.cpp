#include "F3Tonemap.pass.hpp"
#include "F4Fill.pass.hpp"
#include "F4Readback.pass.hpp"
#include "NativeGraphSupport.hpp"

namespace native_graph_test
{
    NativeGraphStatistics aliasCase(
        const std::filesystem::path& shaders,
        Validation& validation,
        bool allow_alias,
        EAliasScenario scenario
    )
    {
        auto host = instance(validation);
        DeviceOptions options;
        options.dynamic_rendering = true;
        options.multiple_queues = scenario == EAliasScenario::MULTI_QUEUE;
        auto device = checked(VulkanDevice::create(host, options));
        auto allocator = checked(VulkanAllocator::create(device));
        auto queue = checked(SubmissionQueue::create(device, 64));
        auto compute_queue = checked(SubmissionQueue::create(device, 64, EQueueRole::COMPUTE));
        auto transfer_queue = checked(SubmissionQueue::create(device, 64, EQueueRole::TRANSFER));
        auto sampler = checked(Sampler::create(device));
        RenderGraphBuilder builder;
        std::vector<NativePassCommand> commands;
        std::array<GraphPassId, 2> compute, graphics;
        std::array<GraphResourceId, 2> outputs;
        for (unsigned chain = 0; chain < 2; ++chain)
        {
            const auto prefix = std::string("chain.") + std::to_string(chain);
            TextureDesc desc;
            desc.width = desc.height = 4;
            desc.mip_count = scenario == EAliasScenario::INCOMPATIBLE && chain == 1 ? 2 : 1;
            F4Fill fill;
            fill.output.texture = builder.texture(desc, prefix + ".hdr");
            F3Tonemap tonemap;
            tonemap.input.texture = fill.output.texture;
            tonemap.input.range = {};
            tonemap.nearest.sampler = GraphSampler{1};
            tonemap.output.texture = builder.texture(desc, prefix + ".ldr");
            F4Readback read;
            read.source.texture = tonemap.output.texture;
            read.destination.buffer = builder.buffer({256, 4}, prefix + ".readback");
            outputs[chain] = GraphResourceId{read.destination.buffer.value()};
            compute[chain] =
                checked(builder.compute(prefix + ".fill", {{"F4Fill", "default"}}, EExecutionScope::VIEW, fill));
            graphics[chain] = checked(
                builder.graphics(prefix + ".tonemap", {{"F3Tonemap", "default"}}, EExecutionScope::VIEW, tonemap)
            );
            auto transfer = checked(builder.transfer(prefix + ".readback", EExecutionScope::VIEW, read));
            CHECK(builder.exportBuffer(
                read.destination.buffer,
                PassProducer{passKey(prefix + ".readback")},
                EGraphOutput::READBACK
            ));
            commands.push_back(
                {compute[chain],
                 DispatchCommand{4, 4, 1},
                 options.multiple_queues ? EQueueRole::COMPUTE : EQueueRole::GRAPHICS}
            );
            commands.push_back({graphics[chain], DrawCommand{}, EQueueRole::GRAPHICS});
            commands.push_back(
                {transfer, CopyCommand{}, options.multiple_queues ? EQueueRole::TRANSFER : EQueueRole::GRAPHICS}
            );
        }
        if (scenario == EAliasScenario::OVERLAP)
        {
            CHECK(builder.after(passKey("chain.0.fill"), passKey("chain.1.fill")));
            CHECK(builder.after(passKey("chain.1.fill"), passKey("chain.0.tonemap")));
            CHECK(builder.after(passKey("chain.0.tonemap"), passKey("chain.1.tonemap")));
            CHECK(builder.after(passKey("chain.1.tonemap"), passKey("chain.0.readback")));
            CHECK(builder.after(passKey("chain.0.readback"), passKey("chain.1.readback")));
        }
        else
        {
            CHECK(builder.after(passKey("chain.0.readback"), passKey("chain.1.fill")));
        }
        auto definition = checked(std::move(builder).finish());
        auto logical = checked(compileLogicalGraph(definition));
        auto invocation = makeGraphInvocationData(definition);
        std::vector<NativeShaderProgram> programs;
        for (unsigned chain = 0; chain < 2; ++chain)
        {
            programs.push_back(
                program(device, logical, compute[chain], "F4Fill", PassSchema<F4Fill>::contract(), shaders, true)
            );
            programs.push_back(program(
                device,
                logical,
                graphics[chain],
                "F3Tonemap",
                PassSchema<F3Tonemap>::contract(),
                shaders,
                false
            ));
        }
        const std::array samplers{std::pair{GraphSampler{1}, std::cref(sampler)}};
        NativeCompileInputs inputs{
            logical,
            device,
            allocator,
            {queue.get(), compute_queue.get(), transfer_queue.get()},
            commands,
            {},
            invocation,
            samplers,
            3,
            allow_alias
        };
        auto executable = checked(compileVulkanGraph(inputs, std::move(programs)));
        std::array<GraphImportBinding, 0> imports;
        std::array<std::optional<GraphSubmission>, 3> receipts;
        for (unsigned slot = 0; slot < 3; ++slot)
        {
            auto frame =
                checked(FrameGraphBindings::create(executable.logical(), {slot, slot, slot}, imports, invocation));
            receipts[slot] = checked(executable.submit(frame, {}));
        }
        CHECK(checked(queue->wait(receipts[2]->completion, UINT64_MAX)));
        for (const auto& receipt : receipts)
        {
            std::array<float, 64> a{}, b{};
            CHECK(executable.readback(*receipt, outputs[0], 0, std::as_writable_bytes(std::span{a})));
            CHECK(executable.readback(*receipt, outputs[1], 0, std::as_writable_bytes(std::span{b})));
            CHECK(a == b);
            for (unsigned y = 0; y < 4; ++y)
            {
                for (unsigned x = 0; x < 4; ++x)
                {
                    CHECK(std::abs(a[(y * 4 + x) * 4] - float(x + 1) / float(x + 2)) < 1e-6f);
                }
            }
        }
        std::printf("NATIVE_ALLOCATION alias=%u %s\n", unsigned(allow_alias), executable.diagnostics().c_str());
        const auto stats = executable.statistics();
        std::printf(
            "ALIAS scenario=%u enabled=%u allocations=%u bytes=%llu aliases=%u FIF=3\n",
            unsigned(scenario),
            unsigned(allow_alias),
            stats.allocations,
            stats.allocated_bytes,
            stats.aliased_resources
        );
        return stats;
    }
} // namespace native_graph_test
