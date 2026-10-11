#include "F4Accumulate.pass.hpp"
#include "F4Fill.pass.hpp"
#include "F4HostRead.pass.hpp"
#include "NativeGraphSupport.hpp"

namespace native_graph_test
{
    void readWriteCase(const std::filesystem::path& shaders, Validation& validation)
    {
        auto host = instance(validation);
        auto device = checked(VulkanDevice::create(host));
        auto allocator = checked(VulkanAllocator::create(device));
        auto queue = checked(SubmissionQueue::create(device, 24));
        RenderGraphBuilder builder;
        TextureDesc desc;
        desc.width = desc.height = 4;
        F4Fill fill;
        fill.output.texture = builder.texture(desc, "read_write.image");
        const auto first = checked(builder.compute("rw.0", {{"F4Fill", "default"}}, EExecutionScope::VIEW, fill));
        std::vector<NativePassCommand> commands{{first, DispatchCommand{4, 4, 1}, EQueueRole::GRAPHICS}};
        std::array<GraphPassId, 2> updates;
        std::array<GraphPassId, 3> reads;
        for (unsigned i = 0; i < 3; ++i)
        {
            const auto name = "rw." + std::to_string(i);
            if (i != 0)
            {
                F4Accumulate accumulate;
                accumulate.image.texture = fill.output.texture;
                updates[i - 1] =
                    checked(builder.compute(name, {{"F4Accumulate", "default"}}, EExecutionScope::VIEW, accumulate));
                CHECK(builder.source(passKey(name), "image", PassProducer{passKey("rw." + std::to_string(i - 1))}));
                CHECK(builder.after(passKey("rw.read." + std::to_string(i - 1)), passKey(name)));
                commands.push_back({updates[i - 1], DispatchCommand{4, 4, 1}, EQueueRole::GRAPHICS});
            }
            F4HostRead read;
            read.source.texture = fill.output.texture;
            const auto read_name = "rw.read." + std::to_string(i);
            reads[i] = checked(builder.readback(read_name, EExecutionScope::VIEW, read));
            CHECK(builder.source(passKey(read_name), "source", PassProducer{passKey(name)}));
            commands.push_back({reads[i], CopyCommand{}, EQueueRole::GRAPHICS});
        }
        auto definition = checked(std::move(builder).finish());
        auto logical = checked(compileLogicalGraph(definition));
        auto invocation = makeGraphInvocationData(definition);
        std::vector<NativeShaderProgram> programs;
        programs.push_back(program(device, logical, first, "F4Fill", PassSchema<F4Fill>::contract(), shaders, true));
        for (auto update : updates)
        {
            programs.push_back(
                program(device, logical, update, "F4Accumulate", PassSchema<F4Accumulate>::contract(), shaders, true)
            );
        }
        auto executable = checked(compileVulkanGraph(
            {logical, device, allocator, {queue.get(), queue.get(), queue.get()}, commands, {}, invocation},
            std::move(programs)
        ));
        std::array<GraphImportBinding, 0> imports;
        for (unsigned serial = 0; serial < 6; ++serial)
        {
            auto frame = checked(
                FrameGraphBindings::create(executable.logical(), {serial, serial, serial % 2}, imports, invocation)
            );
            const auto receipt = checked(executable.submit(frame, {}));
            CHECK(checked(queue->wait(receipt.completion, UINT64_MAX)));
            for (unsigned i = 0; i < 3; ++i)
            {
                std::array<float, 64> pixels;
                CHECK(executable.readbackPass(receipt, reads[i], 0, 0, std::as_writable_bytes(std::span{pixels})));
                for (unsigned y = 0; y < 4; ++y)
                {
                    for (unsigned x = 0; x < 4; ++x)
                    {
                        const std::array expected{float(x + 1 + i), float(y + 2 + i), float(3 + i), float(1 + i)};
                        for (unsigned c = 0; c < 4; ++c)
                        {
                            CHECK(pixels[(y * 4 + x) * 4 + c] == expected[c]);
                        }
                    }
                }
            }
        }
        std::puts("READ_WRITE RAW_WAR_WAW intermediate_versions frames=6 CPU_oracle=PASS");
    }
} // namespace native_graph_test
