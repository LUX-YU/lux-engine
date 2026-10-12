#include "F3Tonemap.pass.hpp"
#include "F4Fill.pass.hpp"
#include "F4HostRead.pass.hpp"
#include "NativeGraphSupport.hpp"

using namespace native_graph_test;

int main(int argc, char** argv)
{
    CHECK(argc == 2);
    Validation validation;
    {
        auto host = instance(validation);
        DeviceOptions options;
        options.dynamic_rendering = true;
        auto device = checked(VulkanDevice::create(host, options));
        auto allocator = checked(VulkanAllocator::create(device));
        auto queue = checked(SubmissionQueue::create(device, 64));
        auto sampler = checked(Sampler::create(device));
        RenderGraphBuilder builder;
        TextureDesc texture;
        texture.extent_kind = EExtentKind::TARGET_RELATIVE;
        const auto hdr = builder.texture(texture, "relative.hdr");
        const auto ldr = builder.texture(texture, "relative.ldr");
        F4Fill fill;
        fill.output.texture = hdr;
        const auto compute = checked(builder.compute("fill", {{"F4Fill", "default"}}, EExecutionScope::VIEW, fill));
        F3Tonemap tonemap;
        tonemap.input.texture = hdr;
        tonemap.nearest.sampler = GraphSampler{1};
        tonemap.output.texture = ldr;
        const auto graphics =
            checked(builder.graphics("tonemap", {{"F3Tonemap", "default"}}, EExecutionScope::VIEW, tonemap));
        F4HostRead read;
        read.source.texture = ldr;
        const auto transfer = checked(builder.readback("readback", EExecutionScope::VIEW, read));
        CHECK(builder.source(passKey("readback"), "source", PassProducer{passKey("tonemap")}));
        auto definition = checked(std::move(builder).finish());
        auto logical = checked(compileLogicalGraph(definition));
        auto values = makeGraphInvocationData(definition);
        std::vector<NativeShaderProgram> programs;
        programs.push_back(program(device, logical, compute, "F4Fill", PassSchema<F4Fill>::contract(), argv[1], true));
        programs.push_back(
            program(device, logical, graphics, "F3Tonemap", PassSchema<F3Tonemap>::contract(), argv[1], false)
        );
        std::array commands{
            NativePassCommand{compute, DispatchCommand{4, 4, 1}, EQueueRole::GRAPHICS},
            NativePassCommand{graphics, DrawCommand{}, EQueueRole::GRAPHICS},
            NativePassCommand{transfer, CopyCommand{}, EQueueRole::GRAPHICS}
        };
        std::optional<Graph> first{checked(compileVulkanGraph(
            {logical, device, {queue.get(), queue.get(), queue.get()}, commands, true, {4, 4}},
            std::move(programs)
        ))};
        const std::array samplers{std::pair{GraphSampler{1}, std::cref(sampler)}};
        auto old = checked(GraphExecutionContext::create(*first, {device, allocator, {}, values, samplers, 2}));
        std::array<GraphImportBinding, 0> imports;
        const auto old_bindings = checked(FrameGraphBindings::create(old.logical(), {1, 0, 0}, imports, values));
        const auto old_receipt = checked(executeGraph(old, 0, old_bindings, {}));
        const auto zero = specializeVulkanGraph(
            {logical, device, {queue.get(), queue.get(), queue.get()}, commands, true, {0, 0}},
            *first
        );
        CHECK(!zero && zero.error().type == kInvalidArgument);
        commands[0].command = DispatchCommand{6, 3, 1};
        auto resized = checked(specializeVulkanGraph(
            {logical, device, {queue.get(), queue.get(), queue.get()}, commands, true, {6, 3}},
            *first
        ));
        CHECK(resized.shaderPrograms().data() == first->shaderPrograms().data());
        CHECK(resized.targetExtent().width == 6 && resized.targetExtent().height == 3);
        auto current = checked(GraphExecutionContext::create(resized, {device, allocator, {}, values, samplers, 3}));
        first.reset(); // Both the old in-flight invocation and the new code retain the same native programs.
        const auto new_bindings = checked(FrameGraphBindings::create(current.logical(), {2, 0, 0}, imports, values));
        const auto receipt = checked(executeGraph(current, 0, new_bindings, {}));
        CHECK(checked(queue->wait(receipt.completion, UINT64_MAX)));
        const auto verify =
            [&](GraphExecutionContext& invocations, GraphSubmission completion, unsigned width, unsigned height)
        {
            std::vector<float> pixels(width * height * 4);
            CHECK(invocations.readbackPass(completion, transfer, 0, 0, std::as_writable_bytes(std::span{pixels})));
            for (unsigned y = 0; y < height; ++y)
            {
                for (unsigned x = 0; x < width; ++x)
                {
                    const std::array<float, 4> input{float(x + 1), float(y + 2), 3, 1};
                    for (unsigned c = 0; c < 4; ++c)
                    {
                        const float expected = c == 3 ? 1 : input[c] / (1 + input[c]);
                        CHECK(std::abs(pixels[(y * width + x) * 4 + c] - expected) < 1e-6f);
                    }
                }
            }
        };
        verify(old, old_receipt, 4, 4);
        verify(current, receipt, 6, 3);
    }
    CHECK(validation.errors == 0);
    std::printf("TARGET_EXTENT 4x4_to_6x3 old_inflight=PASS shared_shader_owners=PASS CPU_pixel_oracle=PASS "
                "validation_errors=0\n");
}
