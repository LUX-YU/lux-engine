#include "F4Raster.pass.hpp"
#include "F4Readback.pass.hpp"
#include "NativeGraphSupport.hpp"

namespace native_graph_test
{
    void rasterCase(const std::filesystem::path& shaders, Validation& validation)
    {
        auto host = instance(validation);
        DeviceOptions options;
        options.dynamic_rendering = true;
        options.separate_depth_stencil = true;
        auto device = checked(VulkanDevice::create(host, options));
        auto allocator = checked(VulkanAllocator::create(device));
        auto queue = checked(SubmissionQueue::create(device, 32));
        RenderGraphBuilder builder;
        TextureDesc color;
        color.width = color.height = 4;
        const auto resolved = builder.texture(color, "resolve.single");
        color.samples = 4;
        const auto msaa = builder.texture(color, "resolve.multisampled");
        color.format = lux::rdesc::ETextureFormat::D24_UNORM_S8_UINT;
        const auto depth = builder.texture(color, "depth.stencil");
        std::array<GraphPassId, 3> draws;
        std::array<GraphBuffer, 3> observed;
        std::vector<NativePassCommand> commands;
        for (unsigned draw = 0; draw < 3; ++draw)
        {
            F4Raster params;
            params.red = draw == 0 ? 1.0f : 0.0f;
            params.green = draw == 1 ? 1.0f : 0.0f;
            params.blue = draw == 2 ? 1.0f : 0.0f;
            params.depth_value = draw == 1 ? 0.75f : draw == 0 ? 0.25f : 0.125f;
            params.output.texture = msaa;
            params.output.load = draw == 0 ? ELoadOp::CLEAR : ELoadOp::LOAD;
            params.resolved.texture = resolved;
            params.depth.texture = depth;
            params.depth.range.aspect = EAspect::DEPTH_STENCIL;
            params.depth.load = params.depth.stencil_load = draw == 0 ? ELoadOp::CLEAR : ELoadOp::LOAD;
            params.depth.stencil_store = EStoreOp::STORE;
            const auto name = "raster." + std::to_string(draw);
            draws[draw] = checked(builder.graphics(name, {{"F4Raster", "default"}}, EExecutionScope::VIEW, params));
            commands.push_back({draws[draw], DrawCommand{}, EQueueRole::GRAPHICS});
            F4Readback checkpoint;
            checkpoint.source.texture = resolved;
            checkpoint.destination.buffer = observed[draw] = builder.buffer({256, 4}, name + ".pixels");
            const auto read_name = name + ".read";
            const auto checkpoint_pass = checked(builder.transfer(read_name, EExecutionScope::VIEW, checkpoint));
            CHECK(builder.source(passKey(read_name), "source", PassProducer{passKey(name)}));
            CHECK(builder.exportBuffer(observed[draw], PassProducer{passKey(read_name)}, EGraphOutput::READBACK));
            commands.push_back({checkpoint_pass, CopyCommand{}, EQueueRole::GRAPHICS});
            if (draw != 0)
            {
                const auto previous = passKey("raster." + std::to_string(draw - 1));
                CHECK(builder.after(passKey("raster." + std::to_string(draw - 1) + ".read"), passKey(name)));
                CHECK(builder.source(passKey(name), "output", PassProducer{previous}));
                CHECK(builder.source(passKey(name), "depth", PassProducer{previous}));
            }
        }
        F4Readback read;
        read.source.texture = resolved;
        read.destination.buffer = builder.buffer({256, 4}, "raster.bytes");
        const auto copy = checked(builder.transfer("raster.read", EExecutionScope::VIEW, read));
        CHECK(builder.source(passKey("raster.read"), "source", PassProducer{passKey("raster.2")}));
        CHECK(
            builder.exportBuffer(read.destination.buffer, PassProducer{passKey("raster.read")}, EGraphOutput::READBACK)
        );
        commands.push_back({copy, CopyCommand{}, EQueueRole::GRAPHICS});
        auto definition = checked(std::move(builder).finish());
        auto logical = checked(compileLogicalGraph(definition));
        auto invocation = makeGraphInvocationData(definition);
        std::vector<NativeShaderProgram> programs;
        for (unsigned draw = 0; draw < 3; ++draw)
        {
            GraphicsDescription graphics;
            graphics.color_formats = {VK_FORMAT_R32G32B32A32_SFLOAT};
            graphics.blends.resize(1);
            graphics.samples = VK_SAMPLE_COUNT_4_BIT;
            graphics.depth_format = graphics.stencil_format = VK_FORMAT_D24_UNORM_S8_UINT;
            graphics.depth_test = graphics.depth_write = graphics.stencil_test = true;
            graphics.front.reference = graphics.back.reference = 7;
            graphics.front.pass = graphics.back.pass = draw == 0 ? VK_STENCIL_OP_REPLACE : VK_STENCIL_OP_KEEP;
            graphics.front.compare = graphics.back.compare = draw == 0 ? VK_COMPARE_OP_ALWAYS : VK_COMPARE_OP_EQUAL;
            programs.push_back(program(
                device,
                logical,
                draws[draw],
                "F4Raster",
                PassSchema<F4Raster>::contract(),
                shaders,
                false,
                0,
                &graphics
            ));
        }
        NativeCompileInputs
            inputs{logical, device, allocator, {queue.get(), queue.get(), queue.get()}, commands, {}, invocation};
        auto executable = checked(compileVulkanGraph(inputs, std::move(programs)));
        std::array<GraphImportBinding, 0> imports;
        auto frame = checked(FrameGraphBindings::create(executable.logical(), {}, imports, invocation));
        auto receipt = checked(executable.submit(frame, {}));
        CHECK(checked(queue->wait(receipt.completion, UINT64_MAX)));
        std::array<float, 64> pixels;
        CHECK(executable.readback(
            receipt,
            GraphResourceId{read.destination.buffer.value()},
            0,
            std::as_writable_bytes(std::span{pixels})
        ));
        for (unsigned pixel = 0; pixel < 16; ++pixel)
        {
            CHECK(
                pixels[pixel * 4] == 0 && pixels[pixel * 4 + 1] == 0 && pixels[pixel * 4 + 2] == 1 &&
                pixels[pixel * 4 + 3] == 1
            );
        }
        for (unsigned i = 0; i < 3; ++i)
        {
            CHECK(executable.readback(
                receipt,
                GraphResourceId{observed[i].value()},
                0,
                std::as_writable_bytes(std::span{pixels})
            ));
            for (unsigned pixel = 0; pixel < 16; ++pixel)
            {
                CHECK(pixels[pixel * 4] == (i < 2 ? 1.0f : 0.0f));
                CHECK(pixels[pixel * 4 + 1] == 0.0f);
                CHECK(pixels[pixel * 4 + 2] == (i < 2 ? 0.0f : 1.0f));
            }
        }
        std::puts("RASTER clear_load_store depth_stencil msaa4_resolve CPU_oracle=PASS");
    }
} // namespace native_graph_test
