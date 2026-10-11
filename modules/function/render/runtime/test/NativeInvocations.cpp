#include "Allocation.hpp"
#include "F3Tonemap.pass.hpp"
#include "F4Fill.pass.hpp"
#include "F4Readback.pass.hpp"
#include "NativeGraphSupport.hpp"
#include <cstring>

using namespace native_graph_test;

void invocationCase(const std::filesystem::path& shaders, Validation& validation, unsigned frames, bool sharing)
{
    auto host = instance(validation);
    DeviceOptions options;
    options.dynamic_rendering = true;
    auto device = checked(VulkanDevice::create(host, options));
    auto allocator = checked(VulkanAllocator::create(device));
    auto queue = checked(SubmissionQueue::create(device, 128));
    auto sampler = checked(Sampler::create(device));
    RenderGraphBuilder builder;
    TextureDesc texture;
    texture.width = texture.height = 4;
    const auto hdr = builder.texture(texture, "invocation.hdr");
    const auto ldr = builder.texture(texture, "invocation.ldr");
    const auto output = builder.buffer({256, 4}, "invocation.readback");
    F4Fill fill;
    fill.output.texture = hdr;
    const auto compute = checked(
        builder.compute("fill", {{"F4Fill", "default"}}, sharing ? EExecutionScope::SCENE : EExecutionScope::VIEW, fill)
    );
    F3Tonemap tonemap;
    tonemap.input.texture = hdr;
    tonemap.nearest.sampler = GraphSampler{1};
    tonemap.output.texture = ldr;
    const auto graphics =
        checked(builder.graphics("tonemap", {{"F3Tonemap", "default"}}, EExecutionScope::VIEW, tonemap));
    F4Readback read;
    read.source.texture = ldr;
    read.destination.buffer = output;
    const auto transfer = checked(builder.transfer("readback", EExecutionScope::VIEW, read));
    CHECK(builder.source(passKey("readback"), "source", PassProducer{passKey("tonemap")}));
    CHECK(builder.exportBuffer(output, PassProducer{passKey("readback")}, EGraphOutput::READBACK));
    auto definition = checked(std::move(builder).finish());
    auto logical = checked(compileLogicalGraph(definition));
    auto initial = makeGraphInvocationData(definition);
    std::vector<NativeShaderProgram> programs;
    programs.push_back(program(device, logical, compute, "F4Fill", PassSchema<F4Fill>::contract(), shaders, true));
    programs.push_back(
        program(device, logical, graphics, "F3Tonemap", PassSchema<F3Tonemap>::contract(), shaders, false)
    );
    const std::array commands{
        NativePassCommand{compute, DispatchCommand{4, 4, 1}, EQueueRole::GRAPHICS},
        NativePassCommand{graphics, DrawCommand{}, EQueueRole::GRAPHICS},
        NativePassCommand{transfer, CopyCommand{}, EQueueRole::GRAPHICS}
    };
    auto plan = checked(
        compileVulkanGraph({logical, device, {queue.get(), queue.get(), queue.get()}, commands}, std::move(programs))
    );
    const std::array samplers{std::pair{GraphSampler{1}, std::cref(sampler)}};
    const unsigned capacity = 4 * frames;
    auto invocations =
        checked(NativeGraphInvocations::create(plan, {device, allocator, {}, initial, samplers, capacity}));
    auto other = checked(NativeGraphInvocations::create(plan, {device, allocator, {}, initial, samplers, 1}));
    CHECK(&invocations.logical() == &plan.logical() && &other.logical() == &plan.logical());
    std::array<GraphInvocationData, 4> values{initial, initial, initial, initial};
    const auto& fields = logical.identity().passes[graphics.value() - 1].scalar_fields;
    CHECK(fields.size() == 1 && fields[0].size == sizeof(float));
    for (unsigned view = 0; view < 4; ++view)
    {
        values[view].scene = 1;
        values[view].view = view + 1;
        values[view].target = view + 1;
        values[view].history_epoch = view + 1;
        values[view].camera[0] = float(view + 1);
        const float exposure = float(view + 1);
        std::memcpy(
            values[view].passes[graphics.value() - 1].scalars.data() + fields[0].offset,
            &exposure,
            sizeof(exposure)
        );
    }
    std::array<GraphImportBinding, 0> imports;
    std::array<std::optional<GraphSubmission>, 12> receipts;
    std::vector<NativeTraceEvent> trace(invocations.traceCapacity());
    for (unsigned round = 0; round < 25; ++round)
    {
        allocation_count = allocation_bytes = 0;
        measuring = round != 0;
        for (unsigned frame = 0; frame < frames; ++frame)
        {
            for (unsigned view = 0; view < 4; ++view)
            {
                const auto slot = frame * 4 + view;
                const auto bindings = checked(FrameGraphBindings::create(
                    plan.logical(),
                    {round * frames + frame + 1, 0, frame},
                    imports,
                    values[view]
                ));
                const auto source = sharing && view != 0 ? receipts[frame * 4] : std::optional<GraphSubmission>{};
                receipts[slot] = checked(invocations.submit(slot, bindings, {}, trace, source));
                unsigned executed = 0, reused = 0;
                for (unsigned t = 0; t < receipts[slot]->trace_count; ++t)
                {
                    if (trace[t].pass == compute)
                    {
                        executed += trace[t].operation == ENativeTrace::PASS;
                        reused += trace[t].operation == ENativeTrace::REUSE_SCENE;
                    }
                }
                CHECK(executed == (source ? 0u : 1u) && reused == (source ? 1u : 0u));
            }
        }
        measuring = false;
        CHECK(allocation_count == 0 && allocation_bytes == 0);
        CHECK(checked(queue->wait(receipts[capacity - 1]->completion, UINT64_MAX)));
        // Waiting only for the final join preserves every earlier independent receipt.
        for (unsigned slot = 0; slot < capacity; ++slot)
        {
            std::array<float, 64> pixels{};
            CHECK(invocations.readback(
                *receipts[slot],
                GraphResourceId{output.value()},
                0,
                std::as_writable_bytes(std::span{pixels})
            ));
            const float exposure = float(slot % 4 + 1);
            for (unsigned y = 0; y < 4; ++y)
            {
                for (unsigned x = 0; x < 4; ++x)
                {
                    const std::array<float, 4> hdr_pixel{float(x + 1), float(y + 2), 3.0f, 1.0f};
                    for (unsigned channel = 0; channel < 4; ++channel)
                    {
                        const float h = hdr_pixel[channel] * exposure;
                        const float expected = channel == 3 ? 1.0f : h / (1.0f + h);
                        CHECK(std::abs(pixels[(y * 4 + x) * 4 + channel] - expected) < 1e-6f);
                    }
                }
            }
        }
        const auto wrong = other.completed(*receipts[0]);
        CHECK(!wrong && wrong.error().type == kWrongOwner);
    }
    if (sharing)
    {
        const auto frame = GraphFrameValues{1000, 0, 0};
        const auto first = checked(FrameGraphBindings::create(plan.logical(), frame, imports, values[0]));
        const auto donor = checked(invocations.submit(0, first, {}));
        ++values[1].scene_revision;
        const auto changed = checked(FrameGraphBindings::create(plan.logical(), frame, imports, values[1]));
        const auto refused = invocations.submit(1, changed, {}, {}, donor);
        CHECK(!refused && refused.error().type == kInvalidArgument);
        --values[1].scene_revision;
        // The donor's accepted snapshot survives host-side source mutation.
        ++values[0].scene_revision;
        const auto compatible = checked(FrameGraphBindings::create(plan.logical(), frame, imports, values[1]));
        const auto reused = checked(invocations.submit(1, compatible, {}, {}, donor));
        CHECK(checked(queue->wait(reused.completion, UINT64_MAX)));
    }
    std::printf(
        "NATIVE_INVOCATIONS views=4 FIF=%u capacity=%u plans=1 scene_reuse=%u pixel_oracles=PASS "
        "warmed_allocations=0\n",
        frames,
        capacity,
        sharing
    );
}

int main(int argc, char** argv)
{
    CHECK(argc == 2);
    Validation validation;
    invocationCase(argv[1], validation, 2, false);
    invocationCase(argv[1], validation, 2, true);
    invocationCase(argv[1], validation, 3, false);
    invocationCase(argv[1], validation, 3, true);
    CHECK(validation.errors == 0);
    std::printf("validation_errors=%u\n", validation.errors.load());
}
