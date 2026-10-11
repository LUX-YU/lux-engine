#include "Allocation.hpp"
#include "F5CReadBuffer.pass.hpp"
#include "NativeGraphSupport.hpp"
#include <array>

using namespace native_graph_test;

namespace
{
    std::array<double, 256> timings{};
    unsigned recorded{};
    bool capture{};
} // namespace

void recordRuntimeImportProfile(double ns) noexcept
{
    if (capture)
    {
        CHECK(recorded < timings.size());
        timings[recorded++] = ns;
    }
}

int main()
{
    Validation validation;
    auto host = instance(validation);
    auto device = checked(VulkanDevice::create(host));
    auto allocator = checked(VulkanAllocator::create(device));
    auto queue = checked(SubmissionQueue::create(device, 2048));
    for (unsigned resources : {1, 8, 32, 64})
    {
        RenderGraphBuilder builder;
        std::vector<Buffer> buffers;
        std::vector<NativeImportBinding> native;
        std::vector<GraphImportBinding> imports;
        std::vector<NativePassCommand> commands;
        buffers.reserve(resources);
        for (unsigned i = 0; i < resources; ++i)
        {
            const auto id = builder.importBuffer("scale." + std::to_string(i), {32, 4}, EPersistentScope::RUNTIME);
            buffers.push_back(
                checked(Buffer::create(allocator, 32, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, EMemoryAccess::UPLOAD))
            );
            std::array<std::uint32_t, 8> values;
            for (unsigned j = 0; j < values.size(); ++j)
            {
                values[j] = i * 100 + j;
            }
            CHECK(buffers.back().write(0, std::as_bytes(std::span{values})));
            native.push_back(
                {GraphResourceId{id.value()},
                 GraphBackingId{i + 1},
                 std::cref(buffers.back()),
                 {VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_WRITE_BIT},
                 {},
                 true}
            );
            imports.push_back({GraphResourceId{id.value()}, GraphBackingId{i + 1}, 0, 1});
            for (unsigned part = 0; part < 2; ++part)
            {
                F5CReadBuffer params;
                params.source = {id, {part * 16, 16}};
                const auto pass = checked(builder.readback(
                    "read." + std::to_string(i) + "." + std::to_string(part),
                    EExecutionScope::VIEW,
                    params
                ));
                commands.push_back({pass, CopyCommand{}, EQueueRole::GRAPHICS});
            }
        }
        auto definition = checked(std::move(builder).finish());
        auto logical = checked(compileLogicalGraph(definition));
        auto values = makeGraphInvocationData(definition);
        auto code =
            checked(compileVulkanGraph({logical, device, {queue.get(), queue.get(), queue.get()}, commands}, {}));
        for (unsigned views : {1, 4})
        {
            for (unsigned fif : {2, 3})
            {
                auto invocations =
                    checked(NativeGraphInvocations::create(code, {device, allocator, native, values, {}, views * fif}));
                recorded = 0;
                allocation_count = allocation_bytes = 0;
                for (unsigned serial = 0; serial < 72; ++serial)
                {
                    capture = serial >= 8;
                    for (unsigned view = 0; view < views; ++view)
                    {
                        const auto slot = view * fif + serial % fif;
                        values.view = view + 1;
                        auto frame = checked(
                            FrameGraphBindings::create(code.logical(), {serial, serial, serial % fif}, imports, values)
                        );
                        measuring = capture;
                        auto receipt = checked(invocations.submit(slot, frame, native));
                        measuring = false;
                        CHECK(checked(queue->wait(receipt.completion, UINT64_MAX)));
                        if (serial == 71)
                        {
                            for (unsigned i = 0; i < resources; ++i)
                            {
                                for (unsigned part = 0; part < 2; ++part)
                                {
                                    std::array<std::uint32_t, 4> actual{};
                                    CHECK(invocations.readbackPass(
                                        receipt,
                                        commands[i * 2 + part].pass,
                                        0,
                                        0,
                                        std::as_writable_bytes(std::span{actual})
                                    ));
                                    for (unsigned j = 0; j < actual.size(); ++j)
                                    {
                                        CHECK(actual[j] == i * 100 + part * 4 + j);
                                    }
                                }
                            }
                        }
                    }
                }
                capture = false;
                CHECK(recorded == 64 * views && allocation_count == 0 && allocation_bytes == 0);
                auto sorted = timings;
                std::sort(sorted.begin(), sorted.begin() + recorded);
                std::printf(
                    "{\"imports\":%u,\"parts_per_import\":2,\"views\":%u,\"fif\":%u,"
                    "\"p50_ns\":%.0f,\"p95_ns\":%.0f,\"allocations\":0,\"allocation_bytes\":0,\"samples_ns\":[",
                    resources,
                    views,
                    fif,
                    sorted[recorded / 2],
                    sorted[recorded * 95 / 100]
                );
                for (unsigned i = 0; i < recorded; ++i)
                {
                    std::printf("%s%.0f", i ? "," : "", timings[i]);
                }
                std::puts("]}");
            }
        }
    }
    CHECK(validation.errors == 0);
}
