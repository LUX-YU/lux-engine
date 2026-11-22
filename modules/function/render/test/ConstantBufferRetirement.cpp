#include "BufferReadback.hpp"
#include "NativeDevice.hpp"
#include "RetirementFence.hpp"
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/extensions/upload/ResourceUploader.hpp>
#include <lux/engine/render/graph/PreparationPlan.hpp>
#include <lux/engine/render/resource/ConstantBufferStorage.hpp>

using namespace foundation_test;

int main(int argc, char** argv)
{
    auto execution = lux::execution::ExecutionRuntime::create({2, 128, {128}});
    CHECK(execution);
    Validation validation;
    DeviceOptions options;
    const bool d3d12 = argc > 1 && std::string_view{argv[1]} == "d3d12";
    options.backend = d3d12 ? EGraphicsBackend::D3D12 : EGraphicsBackend::VULKAN;
    options.multiple_queues = true;
    TestDevice device(options, diagnostics(validation), nativeValidationEnabled());
    auto& graphics = device->queue();
    auto& compute = device->queue(EQueueRole::COMPUTE);
    if (&graphics == &compute) return 77;
    auto uploader = take(ResourceUploader::create(
        *device, TransferOptions{4096, 2}, lux::execution::ExecutionRuntime::instance().cpuScheduler()
    ));
    std::array<std::uint32_t, 4> words{1, 1, 1, 1};
    std::uint64_t revision = 1;
    ConstantBufferStorage storage(*device, sizeof(words), 1);
    const BoundConstantBuffer input{{0}, &storage, {std::as_bytes(std::span{words}), &revision}};
    ConstantBufferSelections round;
    auto completed = device->makeFrontiers();
    const auto prepare = [&]
    {
        device->readCompleted(completed);
        return take(round.prepare(input, completed, *uploader));
    };
    const auto upload = [&]
    {
        CHECK(uploader->hasFrameCopies());
        CHECK(uploader->flushFrameCopies());
        const auto receipt = take(runTask(uploader->submitPreparationCopies()));
        CHECK(receipt && receipt->ranges == 1);
        uploader->acceptFrameCopies(*receipt);
        round.settle(receipt->point, {});
        CHECK(take(graphics.wait(receipt->point.serial, 5'000'000'000)));
        const auto bound = prepare();
        CHECK(bound && bound->buffer);
        return *bound;
    };

    RetirementFence graphics_gate(*device), compute_gate(*device);
    const auto gate = [&](SubmissionQueue& queue, RetirementFence& fence)
    {
        const nri::FenceSubmitDesc wait{fence.fence, 1, nri::StageBits::ALL};
        const auto serial = take(runTask(queue.submit({}, std::span{&wait, 1})));
        return device->submissionPoint(device->queueIndex(queue), serial);
    };
    CHECK(!prepare());
    const auto first = upload();
    const auto compute_point = gate(compute, compute_gate);
    first.queue_uses.accepted->accept(compute_point);
    round.finish(false);
    words.fill(2);
    revision = 2;
    CHECK(!prepare());
    const auto second = upload();
    CHECK(first.buffer != second.buffer);
    second.queue_uses.accepted->accept(gate(graphics, graphics_gate));
    second.queue_uses.accepted->accept(compute_point);
    round.finish(false);

    words.fill(3);
    revision = 3;
    CHECK(!prepare() && !uploader->hasFrameCopies());
    words.fill(4);
    revision = 4;
    graphics_gate.signal();
    CHECK(take(graphics.wait(graphics.submitted(), 5'000'000'000)));
    CHECK(!prepare() && !uploader->hasFrameCopies());
    compute_gate.signal();
    CHECK(take(compute.wait(compute_point.serial, 5'000'000'000)));
    CHECK(!prepare() && uploader->hasFrameCopies());
    // A second consumer arrives before the copy receipt. The receipt must still contain one range.
    const auto peer = take(round.prepare(input, completed, *uploader));
    CHECK(!peer);
    auto* recycled = upload().buffer;
    CHECK(recycled == first.buffer || recycled == second.buffer);
    const auto stored = readBufferWords<4>(*device, *recycled, {nri::AccessBits::CONSTANT_BUFFER, nri::StageBits::ALL});
    const std::array<std::uint32_t, 4> expected{3, 3, 3, 3};
    CHECK(stored == expected);
    round.finish(false);
    revision = 0;
    const auto invalid = round.prepare(input, completed, *uploader);
    CHECK(!invalid && invalid.error().type == kFrameContract && !uploader->hasFrameCopies());
    round.finish(false);
    CHECK(device->drain() && validation.errors == 0);
    std::puts("CONSTANT BUFFER PASS: FIF=1 two backings; graphics alone cannot recycle; "
              "compute released capacity; staged the original blocked revision 3 once after the source advanced to 4");
}
