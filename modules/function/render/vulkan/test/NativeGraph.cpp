#include "F3Tonemap.pass.hpp"
#include "F4Fill.pass.hpp"
#include "F4Local.pass.hpp"
#include "F4Readback.pass.hpp"
#include "NativeGraphSupport.hpp"

using namespace native_graph_test;

int main(int argc, char** argv)
{
#ifdef _MSC_VER
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    CHECK(argc == 2);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::filesystem::path shaders(argv[1]);
    Validation validation;
    copyCase(validation);
    sharedCase(shaders, validation);
    readWriteCase(shaders, validation);
    importedCase(shaders, validation);
    rasterCase(shaders, validation);
    conditionalCase(shaders, validation);
    CHECK(multiviewCase(shaders, validation, false) == multiviewCase(shaders, validation, true));
    hzbCase(shaders, validation);
    for (unsigned mode : {0u, 1u, 2u, 3u, 4u})
    {
        const bool multiple = mode == 1 || mode == 3 || mode == 4;
        const bool local_read = mode == 2 || mode == 4;
        auto host = instance(validation);
        DeviceOptions options;
        options.dynamic_rendering = true;
        options.multiple_queues = multiple;
        options.prefer_dedicated_queues = mode != 3;
        options.local_read = local_read;
        auto device = checked(VulkanDevice::create(host, options));
        auto allocator = checked(VulkanAllocator::create(device));
        auto queue = checked(SubmissionQueue::create(device, 12));
        auto compute_queue = checked(SubmissionQueue::create(device, 12, EQueueRole::COMPUTE));
        auto transfer_queue = checked(SubmissionQueue::create(device, 12, EQueueRole::TRANSFER));
        if (mode == 3)
        {
            CHECK(queue->nativeQueue().family == compute_queue->nativeQueue().family);
            CHECK(queue->nativeQueue().family == transfer_queue->nativeQueue().family);
            CHECK(queue->nativeQueue().handle != compute_queue->nativeQueue().handle);
            CHECK(queue->nativeQueue().handle != transfer_queue->nativeQueue().handle);
            CHECK(compute_queue->nativeQueue().handle != transfer_queue->nativeQueue().handle);
        }
        auto sampler = checked(Sampler::create(device));
        RenderGraphBuilder builder;
        TextureDesc texture;
        texture.width = texture.height = 4;
        const auto hdr = builder.texture(texture, "hdr");
        const auto ldr = builder.texture(texture, "ldr");
        const auto output = builder.buffer({4 * 4 * 4 * sizeof(float), 4}, "readback");
        F4Fill fill;
        fill.output.texture = hdr;
        const auto compute = checked(builder.compute("fill", {{"F4Fill", "default"}}, EExecutionScope::VIEW, fill));
        F3Tonemap tonemap;
        tonemap.input.texture = hdr;
        tonemap.nearest.sampler = GraphSampler{1};
        tonemap.output.texture = ldr;
        F4Local local;
        local.input.texture = hdr;
        local.output.texture = mode == 4 ? hdr : ldr;
        local.output.load = mode == 4 ? ELoadOp::LOAD : ELoadOp::DISCARD;
        const auto graphics =
            local_read
                ? checked(builder.graphics("tonemap", {{"F4Local", "default"}}, EExecutionScope::VIEW, local))
                : checked(builder.graphics("tonemap", {{"F3Tonemap", "default"}}, EExecutionScope::VIEW, tonemap));
        if (mode == 4)
        {
            CHECK(builder.localRead(passKey("tonemap"), "input"));
            CHECK(builder.source(passKey("tonemap"), "input", PassProducer{passKey("fill")}));
            CHECK(builder.source(passKey("tonemap"), "output", PassProducer{passKey("fill")}));
        }
        F4Readback readback;
        readback.source.texture = mode == 4 ? hdr : ldr;
        readback.destination.buffer = output;
        const auto transfer = checked(builder.transfer("readback", EExecutionScope::VIEW, readback));
        CHECK(builder.source(passKey("readback"), "source", PassProducer{passKey("tonemap")}));
        CHECK(builder.exportBuffer(output, PassProducer{passKey("readback")}, EGraphOutput::READBACK));
        auto definition = checked(std::move(builder).finish());
        auto logical = checked(compileLogicalGraph(definition, {.allow_local_read = local_read}));
        auto invocation = makeGraphInvocationData(definition);
        std::vector<NativeShaderProgram> programs;
        programs.push_back(program(device, logical, compute, "F4Fill", PassSchema<F4Fill>::contract(), shaders, true));
        GraphicsDescription local_graphics;
        local_graphics.color_formats = {VK_FORMAT_R32G32B32A32_SFLOAT};
        local_graphics.blends.resize(1);
        local_graphics.color_input_indices = {mode == 4 ? 0u : VK_ATTACHMENT_UNUSED};
        programs.push_back(
            local_read
                ? program(
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
            NativePassCommand{compute, DispatchCommand{4, 4, 1}, multiple ? EQueueRole::COMPUTE : EQueueRole::GRAPHICS},
            NativePassCommand{graphics, DrawCommand{}, EQueueRole::GRAPHICS},
            NativePassCommand{transfer, CopyCommand{}, multiple ? EQueueRole::TRANSFER : EQueueRole::GRAPHICS}
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
        for (std::uint64_t frame = 0; frame < 6; ++frame)
        {
            auto bindings = checked(FrameGraphBindings::create(
                executable.logical(),
                {frame, frame, static_cast<unsigned>(frame % 2)},
                imports,
                invocation
            ));
            auto receipt = checked(executable.submit(bindings, {}));
            CHECK(checked(queue->wait(receipt.completion, UINT64_MAX)));
            std::array<float, 64> pixels{};
            CHECK(executable
                      .readback(receipt, GraphResourceId{output.value()}, 0, std::as_writable_bytes(std::span{pixels}))
            );
            if (frame == 0)
            {
                recordReadback(
                    "W03.queue_mode",
                    mode,
                    GraphResourceId{output.value()},
                    std::as_bytes(std::span{pixels})
                );
            }
            for (unsigned y = 0; y < 4; ++y)
            {
                for (unsigned x = 0; x < 4; ++x)
                {
                    const std::array<float, 4>
                        expected{float(x + 1) / float(x + 2), float(y + 2) / float(y + 3), 0.75f, 1.0f};
                    for (unsigned c = 0; c < 4; ++c)
                    {
                        CHECK(std::abs(pixels[(y * 4 + x) * 4 + c] - expected[c]) < 1e-6f);
                    }
                }
            }
        }
        std::puts(executable.diagnostics().c_str());
        std::printf(
            "QUEUE_MODE multiple=%u local_read=%u same_family=%u\n",
            unsigned(multiple),
            unsigned(local_read),
            unsigned(mode == 3)
        );
    }
    const auto separate = aliasCase(shaders, validation, false);
    const auto aliased = aliasCase(shaders, validation, true);
    CHECK(aliased.aliased_resources == 6 && separate.aliased_resources == 0);
    CHECK(aliased.allocations < separate.allocations && aliased.allocated_bytes < separate.allocated_bytes);
    for (auto scenario : {EAliasScenario::OVERLAP, EAliasScenario::MULTI_QUEUE, EAliasScenario::INCOMPATIBLE})
    {
        CHECK(aliasCase(shaders, validation, true, scenario).aliased_resources == 0);
    }
    CHECK(validation.errors == 0);
    std::printf(
        "PASS F4.0 compute/graphics/readback frames=6 validation_errors=%u warnings=%u\n",
        validation.errors.load(),
        validation.warnings.load()
    );
}
