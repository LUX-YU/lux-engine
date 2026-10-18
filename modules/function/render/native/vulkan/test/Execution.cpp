#include "Support.hpp"
#include "SceneFixture.hpp"
#include "BufferStorageFixture.hpp"
#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>
#include <lux/engine/render/native/vulkan/Readback.hpp>
#include <algorithm>
#include "Allocation.hpp"
#include <chrono>
#include <fstream>

using namespace lux::render;
using namespace lux::render::vulkan;
using namespace foundation_test;
namespace cxx = lux::cxx;

static void pixel(std::span<const std::byte> bytes, unsigned width, unsigned x, unsigned y,
    std::array<unsigned, 4> expected)
{
    for (unsigned i = 0; i < 4; ++i)
    {
        const auto actual = std::to_integer<int>(bytes[(y * width + x) * 4 + i]);
        CHECK(std::abs(actual - int(expected[i])) <= 1);
    }
}

static void readbackOperations(DeviceSession& session, const std::shared_ptr<CompiledGraph>& code,
    const TargetDesc& target)
{
    struct Result final
    {
        unsigned count = 0;
        ReadbackOperation::Result value = cxx::unexpected(RenderError{kBusy});
        static void complete(void* owner, ReadbackOperation::Result value) noexcept
        {
            auto& result = *static_cast<Result*>(owner);
            ++result.count;
            result.value = std::move(value);
        }
    };
    unsigned submitted = 0;
    const auto observed = [](void* value) noexcept { ++*static_cast<unsigned*>(value); };
    auto* context = take(session.prepare(code, target, {}));
    {
        Result first, second;
        ReadbackOperation a{*context, &first, Result::complete, &submitted, observed};
        ReadbackOperation b{*context, &second, Result::complete, &submitted, observed};
        a.start();
        b.start();
        CHECK(submitted == 1 && first.count == 0 && second.count == 0);
        CHECK(take(context->execute()) == EExecutionOutcome::READBACK_HELD);
        CHECK(take(session.queue().wait(*context->lastUse(), UINT64_MAX)));
        CHECK(first.count == 0 && second.count == 0); // Native completion observation does not dispatch.
        session.dispatchCompletions();
        CHECK(first.count == 1 && second.count == 1 && first.value && second.value);
        CHECK(*first.value == *second.value && first.value->size() == target.width * target.height * 4);
        CHECK(session.queue().callbackReservations() == 0);
    }
    {
        Result first, second;
        ReadbackOperation a{*context, &first, Result::complete, nullptr, nullptr};
        ReadbackOperation b{*context, &second, Result::complete, nullptr, nullptr};
        a.start();
        b.start();
        CHECK(session.release(context)); // The completed Context is actually destroyed before callbacks.
        CHECK(first.count == 0 && second.count == 0 && session.queue().callbackReservations() == 2);
        session.dispatchCompletions();
        CHECK(first.count == 1 && second.count == 1 && !first.value && !second.value);
        CHECK(first.value.error().type == kWorkStopped && second.value.error().type == kWorkStopped);
    }
    context = take(session.prepare(code, target, {}));
    {
        auto occupied = std::make_unique<SubmissionCallback[]>(session.queue().callbackCapacity());
        for (std::size_t i = 0; i < session.queue().callbackCapacity(); ++i)
            CHECK(session.queue().reserve(occupied[i]));
        Result result;
        ReadbackOperation failed{*context, &result, Result::complete, &submitted, observed};
        failed.start();
        CHECK(result.count == 1 && !result.value && result.value.error().type == kCapacity);
        CHECK(!context->lastUse() && submitted == 1); // Reservation failure publishes no GPU work.
    }
    {
        Result result;
        ReadbackOperation stopped{*context, &result, Result::complete, &submitted, observed};
        stopped.cancel();
        stopped.start();
        CHECK(result.count == 1 && !result.value && result.value.error().type == kWorkStopped);
        CHECK(!context->lastUse() && session.queue().callbackReservations() == 0);
    }
    CHECK(session.release(context));
    // The accepted batch owns graph backing. Detach needs no retirement record or allocation;
    // it cancels an active Readback while the real submission keeps physical resources alive.
    context = take(session.prepare(code, target));
    {
        std::vector<RetirementReservation> reservations;
        for (unsigned i = 0; i < 128; ++i) reservations.push_back(take(session.retirement().reserve()));
        Result result;
        ReadbackOperation pending{*context, &result, Result::complete, nullptr, nullptr};
        pending.start();
        const auto ticket = *context->lastUse();
        render_allocation::participate = true;
        render_allocation::calls = 0;
        render_allocation::window = true;
        CHECK(session.release(context));
        render_allocation::window = false;
        CHECK(render_allocation::calls == 0 && result.count == 0); // Armed Queue borrow still owns terminal delivery.
        CHECK(take(session.queue().wait(ticket, UINT64_MAX)));
        session.dispatchCompletions();
        CHECK(result.count == 1 && !result.value && result.value.error().type == kWorkStopped);
        session.dispatchCompletions();
        CHECK(result.count == 1); // Cancelled terminal cannot be completed twice.
        CHECK(session.retirement().collect());
        CHECK(session.retirement().pending() == 0);
    }
    std::puts("Native Readback: shared output, explicit callback dispatch, Context detach, capacity and pre-start stop");
}

int main()
{
    Validation validation;
    auto instance = foundation_test::instance(validation);
    DeviceOptions options;
    options.dynamic_rendering = true;
    auto session = take(DeviceSession::create(instance, options));
    CHECK(buffer_fixture::install(*session));
    native_fixture::Config config{32, 32, 1, 1};
    auto assembly = take(native_fixture::assemble(config));
    auto code = std::make_shared<CompiledGraph>(take(compileGraph(*session, (assembly).definition, (assembly).analysis, (assembly).external_bindings,
        native_fixture::programs(config), native_fixture::target(config))));
    StorageSet scene(session->resources());
    StorageSet view_a(session->resources());
    StorageSet view_b(session->resources());
    auto* a = take(session->prepare(code, native_fixture::target(config), &scene, &view_a));
    auto* b = take(session->prepare(code, native_fixture::target(config), &scene, &view_b));
    CHECK(&a->code() == &b->code() && session->resources().size() == 6);
    for (std::uint32_t i = 0; i < code->resources().size(); ++i)
    {
        const auto& resource = code->resources()[i];
        if (!resource.storage) continue;
        const auto& groups = code->storageGroups();
        const bool shared = std::ranges::find(groups.global, i) != groups.global.end() ||
            std::ranges::find(groups.scene, i) != groups.scene.end();
        CHECK((a->nativeBuffer(i) == b->nativeBuffer(i)) == shared);
    }
    std::vector<VkBuffer> original_scopes(code->resources().size());
    for (std::uint32_t i = 0; i < original_scopes.size(); ++i)
        if (code->resources()[i].storage) original_scopes[i] = a->nativeBuffer(i);
    {
        const std::array<float, 4> values{1, 0, 0, 1};
        CHECK(a->setContextParameters(std::as_bytes(std::span(values))));
    }
    {
        const std::array<float, 4> values{0, 1, 0, 1};
        CHECK(b->setContextParameters(std::as_bytes(std::span(values))));
    }
    CHECK(take(a->execute()) == EExecutionOutcome::EXECUTED);
    CHECK(take(b->execute()) == EExecutionOutcome::EXECUTED);
    CHECK(take(session->queue().wait(*b->lastUse(), UINT64_MAX)));
    auto red = take(a->readback()), green = take(b->readback());
    pixel(red, 32, 16, 16, {255, 0, 0, 255});
    pixel(green, 32, 16, 16, {0, 255, 0, 255});
    pixel(red, 32, 0, 0, {32, 64, 128, 255});
    std::array<double, 1000> samples{};
    for (unsigned frame = 0; frame < 20; ++frame)
    {
        CHECK(take(a->execute()) == EExecutionOutcome::EXECUTED);
        CHECK(take(session->queue().wait(*a->lastUse(), UINT64_MAX)));
    }
    render_allocation::participate = true;
    render_allocation::calls.store(0);
    render_allocation::window.store(true);
    for (auto& sample : samples)
    {
        const auto start = std::chrono::steady_clock::now();
        CHECK(take(a->execute()) == EExecutionOutcome::EXECUTED);
        const auto end = std::chrono::steady_clock::now();
        sample = std::chrono::duration<double, std::micro>(end - start).count();
        CHECK(take(session->queue().wait(*a->lastUse(), UINT64_MAX)));
    }
    render_allocation::window.store(false);
    CHECK(render_allocation::calls == 0 && session->resources().size() == 6);
    std::ofstream raw("native-record-execute.csv");
    raw << "sample,record_execute_us\n";
    for (unsigned i = 0; i < samples.size(); ++i) raw << i << ',' << samples[i] << '\n';
    std::sort(samples.begin(), samples.end());
    std::printf("Native CPU record/execute: samples=1000 warmup=20 new_calls=0 p50=%.3f p95=%.3f p99=%.3f max=%.3f us; "
        "GPU Timing=NOT_MEASURED; fence wait outside timing\n", samples[500], samples[950], samples[990], samples[999]);
    for (std::uint32_t i = 0; i < code->resources().size(); ++i)
        if (code->resources()[i].readback && !code->resources()[i].target_sized)
        {
            const auto data = take(a->readback(i));
            CHECK(data.size() == 16 && std::ranges::all_of(data, [](auto byte) { return byte == std::byte{0}; }));
        }
    CHECK(take(a->execute()) == EExecutionOutcome::EXECUTED);
    CHECK(session->release(a));
    CHECK(session->retirement().pending() >= 1); // Context plus any last-use private backing
    CHECK(take(b->execute()) == EExecutionOutcome::EXECUTED);
    CHECK(take(session->queue().wait(*b->lastUse(), UINT64_MAX)));
    pixel(take(b->readback()), 32, 16, 16, {0, 255, 0, 255});
    auto resized = native_fixture::target(config);
    resized.width = 64;
    resized.height = 48;
    auto* larger = take(session->prepare(code, resized, &scene, &view_b));
    for (std::uint32_t i = 0; i < code->resources().size(); ++i)
        if (const auto& storage = code->resources()[i].storage; storage)
            CHECK((b->nativeBuffer(i) == larger->nativeBuffer(i)) == (std::ranges::find(code->storageGroups().attachment, i) == code->storageGroups().attachment.end()));
    {
        const std::array<float, 4> values{0, 0, 1, 1};
        CHECK(larger->setContextParameters(std::as_bytes(std::span(values))));
    }
    CHECK(take(larger->execute()) == EExecutionOutcome::EXECUTED);
    CHECK(take(session->queue().wait(*larger->lastUse(), UINT64_MAX)));
    auto blue = take(larger->readback());
    CHECK(blue.size() == 64 * 48 * 4);
    pixel(blue, 64, 32, 24, {0, 0, 255, 255});
    CHECK(session->release(b));
    CHECK(session->release(larger));
    CHECK(session->drain());
    // No preload: the last Attach lease removes the live identity, including Global/Scene/View.
    CHECK(session->resources().size() == 0);
    const auto creations = session->resources().creations();
    auto* reattached = take(session->prepare(code, native_fixture::target(config), &scene, &view_a));
    const std::array<float, 4> restored_color{1, 0, 0, 1};
    CHECK(reattached->setContextParameters(std::as_bytes(std::span(restored_color))));
    CHECK(session->resources().creations() == creations + 4);
    CHECK(take(reattached->execute()) == EExecutionOutcome::EXECUTED);
    CHECK(take(session->queue().wait(*reattached->lastUse(), UINT64_MAX)));
    pixel(take(reattached->readback()), 32, 16, 16, {255, 0, 0, 255});
    CHECK(session->release(reattached));
    view_a.clear();
    view_b.clear();
    scene.clear();
    CHECK(session->resources().size() == 0);
    code.reset();

    config = {32, 32, 0, 0};
    auto transfer = take(native_fixture::assemble(config));
    auto transfer_code = std::make_shared<CompiledGraph>(take(compileGraph(*session, (transfer).definition, (transfer).analysis, (transfer).external_bindings,
        native_fixture::programs(config), native_fixture::target(config))));
    readbackOperations(*session, transfer_code, native_fixture::target(config));
    auto* clear = take(session->prepare(transfer_code, native_fixture::target(config), {}));
    for (unsigned frame = 0; frame < 4; ++frame)
    {
        CHECK(take(clear->execute()) == EExecutionOutcome::EXECUTED);
        CHECK(take(session->queue().wait(*clear->lastUse(), UINT64_MAX)));
        auto bytes = take(clear->readback());
        pixel(bytes, 32, 0, 0, {32, 64, 128, 255});
        pixel(bytes, 32, 16, 16, {32, 64, 128, 255});
    }
    CHECK(session->release(clear));
    CHECK(session->drain());
    transfer_code.reset();
    session.reset();
    CHECK(validation.errors == 0);
    std::printf("G9 Native pixels: graphics red/green/blue + transfer; scope shared=2 isolated=4; validation=%u warnings=%u\n",
        validation.errors.load(), validation.warnings.load());
}
