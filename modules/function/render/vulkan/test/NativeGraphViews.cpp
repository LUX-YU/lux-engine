#include "F4Readback.pass.hpp"
#include "F4Views.pass.hpp"
#include "NativeGraphSupport.hpp"

namespace native_graph_test
{
    std::array<float, 128> multiviewCase(const std::filesystem::path& shaders, Validation& validation, bool native)
    {
        auto host = instance(validation);
        DeviceOptions options;
        options.dynamic_rendering = true;
        options.multiview = native;
        auto device = checked(VulkanDevice::create(host, options));
        auto allocator = checked(VulkanAllocator::create(device));
        auto queue = checked(SubmissionQueue::create(device, 24));
        RenderGraphBuilder builder;
        TextureDesc texture;
        texture.width = texture.height = 4;
        texture.array_layers = 2;
        const auto target = builder.texture(texture, "views.target");
        std::vector<NativePassCommand> commands;
        std::vector<GraphPassId> graphics;
        for (unsigned view = 0; view < (native ? 1u : 2u); ++view)
        {
            F4Views params;
            params.view_index = view;
            params.output.texture = target;
            params.output.range = {EAspect::COLOR, 0, 1, view, native ? 2u : 1u};
            const auto pass = checked(
                builder
                    .graphics("view." + std::to_string(view), {{"F4Views", "default"}}, EExecutionScope::VIEW, params)
            );
            graphics.push_back(pass);
            commands.push_back({pass, DrawCommand{}, EQueueRole::GRAPHICS});
        }
        F4Readback read;
        read.source = {target, {EAspect::COLOR, 0, 1, 0, 2}};
        read.destination.buffer = builder.buffer({128 * sizeof(float), 4}, "views.readback");
        const auto read_pass = checked(builder.transfer("views.read", EExecutionScope::VIEW, read));
        commands.push_back({read_pass, CopyCommand{}, EQueueRole::GRAPHICS});
        CHECK(builder.exportBuffer(read.destination.buffer, PassProducer{passKey("views.read")}, EGraphOutput::READBACK)
        );
        auto definition = checked(std::move(builder).finish());
        auto logical = checked(compileLogicalGraph(definition));
        auto invocation = makeGraphInvocationData(definition);
        std::vector<NativeShaderProgram> programs;
        for (const auto pass : graphics)
        {
            programs.push_back(program(
                device,
                logical,
                pass,
                "F4Views",
                PassSchema<F4Views>::contract(),
                shaders,
                false,
                native ? 3 : 0
            ));
        }
        NativeCompileInputs
            inputs{logical, device, allocator, {queue.get(), queue.get(), queue.get()}, commands, {}, invocation};
        auto executable = checked(compileVulkanGraph(inputs, std::move(programs)));
        std::array<GraphImportBinding, 0> imports;
        auto frame = checked(FrameGraphBindings::create(executable.logical(), {}, imports, invocation));
        auto receipt = checked(executable.submit(frame, {}));
        CHECK(checked(queue->wait(receipt.completion, UINT64_MAX)));
        std::array<float, 128> pixels;
        CHECK(executable.readback(
            receipt,
            GraphResourceId{read.destination.buffer.value()},
            0,
            std::as_writable_bytes(std::span{pixels})
        ));
        for (unsigned layer = 0; layer < 2; ++layer)
        {
            for (unsigned pixel = 0; pixel < 16; ++pixel)
            {
                const auto offset = layer * 64 + pixel * 4;
                CHECK(pixels[offset] == (layer == 0 ? 0.25f : 0.75f));
                CHECK(pixels[offset + 1] == float(layer));
                CHECK(pixels[offset + 2] == 0 && pixels[offset + 3] == 1);
            }
        }
        std::printf("MULTIVIEW native=%u distinct_layer_values=PASS\n", unsigned(native));
        return pixels;
    }
} // namespace native_graph_test
