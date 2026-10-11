#include "F4Fill.pass.hpp"
#include "F4Hzb.pass.hpp"
#include "F4Readback.pass.hpp"
#include "NativeGraphSupport.hpp"

namespace native_graph_test
{
    void hzbCase(const std::filesystem::path& shaders, Validation& validation)
    {
        auto host = instance(validation);
        auto device = checked(VulkanDevice::create(host));
        auto allocator = checked(VulkanAllocator::create(device));
        auto queue = checked(SubmissionQueue::create(device, 64));
        auto sampler = checked(Sampler::create(device));
        RenderGraphBuilder builder;
        TextureDesc texture;
        texture.width = texture.height = 8;
        texture.mip_count = 4;
        texture.array_layers = 2;
        const auto pyramid = builder.texture(texture, "hzb.pyramid");
        std::vector<NativePassCommand> commands;
        std::array<GraphPassId, 8> producers;
        std::array<GraphResourceId, 8> outputs;
        for (unsigned layer = 0; layer < 2; ++layer)
        {
            for (unsigned mip = 0; mip < 4; ++mip)
            {
                const auto index = layer * 4 + mip;
                const auto name = "hzb." + std::to_string(layer) + "." + std::to_string(mip);
                const ImageRange range{EAspect::COLOR, mip, 1, layer, 1};
                const auto size = 8u >> mip;
                if (mip == 0)
                {
                    F4Fill fill;
                    fill.output = {pyramid, range};
                    producers[index] =
                        checked(builder.compute(name, {{"F4Fill", "default"}}, EExecutionScope::VIEW, fill));
                }
                else
                {
                    F4Hzb reduce;
                    reduce.source = {pyramid, {EAspect::COLOR, mip - 1, 1, layer, 1}};
                    reduce.nearest.sampler = GraphSampler{1};
                    reduce.destination = {pyramid, range};
                    producers[index] =
                        checked(builder.compute(name, {{"F4Hzb", "default"}}, EExecutionScope::VIEW, reduce));
                }
                commands.push_back({producers[index], DispatchCommand{size, size, 1}, EQueueRole::GRAPHICS});
                F4Readback read;
                read.source = {pyramid, range};
                read.destination.buffer = builder.buffer({size * size * 16, 4}, name + ".bytes");
                outputs[index] = GraphResourceId{read.destination.buffer.value()};
                const auto transfer = checked(builder.transfer(name + ".read", EExecutionScope::VIEW, read));
                commands.push_back({transfer, CopyCommand{}, EQueueRole::GRAPHICS});
                CHECK(builder.exportBuffer(
                    read.destination.buffer,
                    PassProducer{passKey(name + ".read")},
                    EGraphOutput::READBACK
                ));
            }
        }
        auto definition = checked(std::move(builder).finish());
        auto logical = checked(compileLogicalGraph(definition));
        auto invocation = makeGraphInvocationData(definition);
        std::vector<NativeShaderProgram> programs;
        for (unsigned i = 0; i < producers.size(); ++i)
        {
            const bool fill = i % 4 == 0;
            programs.push_back(program(
                device,
                logical,
                producers[i],
                fill ? "F4Fill" : "F4Hzb",
                fill ? PassSchema<F4Fill>::contract() : PassSchema<F4Hzb>::contract(),
                shaders,
                true
            ));
        }
        const std::array samplers{std::pair{GraphSampler{1}, std::cref(sampler)}};
        NativeCompileInputs inputs{
            logical,
            device,
            allocator,
            {queue.get(), queue.get(), queue.get()},
            commands,
            {},
            invocation,
            samplers
        };
        auto executable = checked(compileVulkanGraph(inputs, std::move(programs)));
        std::array<GraphImportBinding, 0> imports;
        for (unsigned serial = 0; serial < 4; ++serial)
        {
            auto frame = checked(
                FrameGraphBindings::create(executable.logical(), {serial, serial, serial % 2}, imports, invocation)
            );
            auto receipt = checked(executable.submit(frame, {}));
            CHECK(checked(queue->wait(receipt.completion, UINT64_MAX)));
            for (unsigned i = 0; i < outputs.size(); ++i)
            {
                const auto mip = i % 4;
                const auto size = 8u >> mip;
                std::vector<float> pixels(size * size * 4);
                CHECK(executable.readback(receipt, outputs[i], 0, std::as_writable_bytes(std::span{pixels})));
                for (unsigned y = 0; y < size; ++y)
                {
                    for (unsigned x = 0; x < size; ++x)
                    {
                        const std::array<float, 4> oracle{float((x << mip) + 1), float((y << mip) + 2), 3, 1};
                        for (unsigned channel = 0; channel < 4; ++channel)
                        {
                            CHECK(pixels[(y * size + x) * 4 + channel] == oracle[channel]);
                        }
                    }
                }
            }
        }
        std::printf("HZB one_image=1 mips=4 layers=2 frames=4 CPU_oracle=PASS\n");
    }
} // namespace native_graph_test
