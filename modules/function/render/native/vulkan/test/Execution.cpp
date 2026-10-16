#include "Support.hpp"
#include "SceneFixture.hpp"
#include "BufferStorageFixture.hpp"
#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>
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
    auto scene = take(session->resources().createDomain(session->resources().root(), EResourceScope::SCENE));
    auto view_a = take(session->resources().createDomain(scene, EResourceScope::VIEW));
    auto view_b = take(session->resources().createDomain(scene, EResourceScope::VIEW));
    auto* a = take(session->prepare(code, native_fixture::target(config), view_a));
    auto* b = take(session->prepare(code, native_fixture::target(config), view_b));
    CHECK(&a->code() == &b->code() && session->resources().size() == 6);
    for (std::uint32_t i = 0; i < code->resources().size(); ++i)
    {
        const auto& resource = code->resources()[i];
        if (!resource.storage) continue;
        const bool shared = resource.storage->scope == EResourceScope::GLOBAL ||
            resource.storage->scope == EResourceScope::SCENE;
        CHECK((a->nativeBuffer(i) == b->nativeBuffer(i)) == shared);
    }
    std::vector<VkBuffer> original_scopes(code->resources().size());
    for (std::uint32_t i = 0; i < original_scopes.size(); ++i)
        if (code->resources()[i].storage) original_scopes[i] = a->nativeBuffer(i);
    a->setColor({1, 0, 0, 1});
    b->setColor({0, 1, 0, 1});
    CHECK(take(a->execute()));
    CHECK(take(b->execute()));
    CHECK(take(session->queue().wait(*b->lastUse(), UINT64_MAX)));
    auto red = take(a->readback()), green = take(b->readback());
    pixel(red, 32, 16, 16, {255, 0, 0, 255});
    pixel(green, 32, 16, 16, {0, 255, 0, 255});
    pixel(red, 32, 0, 0, {32, 64, 128, 255});
    std::array<double, 1000> samples{};
    for (unsigned frame = 0; frame < 20; ++frame)
    {
        CHECK(take(a->execute()));
        CHECK(take(session->queue().wait(*a->lastUse(), UINT64_MAX)));
    }
    render_allocation::participate = true;
    render_allocation::calls.store(0);
    render_allocation::window.store(true);
    for (auto& sample : samples)
    {
        const auto start = std::chrono::steady_clock::now();
        CHECK(take(a->execute()));
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
    CHECK(take(a->execute()));
    CHECK(session->release(a));
    CHECK(session->retirement().pending() == 1);
    CHECK(take(b->execute()));
    CHECK(take(session->queue().wait(*b->lastUse(), UINT64_MAX)));
    pixel(take(b->readback()), 32, 16, 16, {0, 255, 0, 255});
    auto resized = native_fixture::target(config);
    resized.width = 64;
    resized.height = 48;
    auto* larger = take(session->prepare(code, resized, view_b));
    for (std::uint32_t i = 0; i < code->resources().size(); ++i)
        if (const auto& storage = code->resources()[i].storage; storage)
            CHECK((b->nativeBuffer(i) == larger->nativeBuffer(i)) == (storage->scope != EResourceScope::ATTACHMENT));
    larger->setColor({0, 0, 1, 1});
    CHECK(take(larger->execute()));
    CHECK(take(session->queue().wait(*larger->lastUse(), UINT64_MAX)));
    auto blue = take(larger->readback());
    CHECK(blue.size() == 64 * 48 * 4);
    pixel(blue, 64, 32, 24, {0, 0, 255, 255});
    CHECK(session->release(b));
    CHECK(session->release(larger));
    CHECK(session->drain());
    // The former leases==0 deletion contract is intentionally replaced by business-scope lifetime.
    CHECK(session->resources().size() == 4); // Global + Scene + two Views, no Attachment-private backing
    const auto creations = session->resources().creations();
    auto* reattached = take(session->prepare(code, native_fixture::target(config), view_a));
    CHECK(session->resources().creations() == creations + 1); // only Attachment-private storage is new
    for (std::uint32_t i = 0; i < original_scopes.size(); ++i)
        if (const auto& storage = code->resources()[i].storage;
            storage && storage->scope != EResourceScope::ATTACHMENT)
            CHECK(reattached->nativeBuffer(i) == original_scopes[i]);
    CHECK(take(reattached->execute()));
    CHECK(take(session->queue().wait(*reattached->lastUse(), UINT64_MAX)));
    pixel(take(reattached->readback()), 32, 16, 16, {255, 0, 0, 255});
    CHECK(session->release(reattached));
    CHECK(session->resources().closeDomain(view_a));
    CHECK(session->resources().size() == 3);
    CHECK(session->resources().closeDomain(scene));
    CHECK(session->resources().size() == 1); // Global survives until session/explicit asset lifetime ends
    code.reset();

    config = {32, 32, 0, 0};
    auto transfer = take(native_fixture::assemble(config));
    auto transfer_code = std::make_shared<CompiledGraph>(take(compileGraph(*session, (transfer).definition, (transfer).analysis, (transfer).external_bindings,
        native_fixture::programs(config), native_fixture::target(config))));
    auto* clear = take(session->prepare(transfer_code, native_fixture::target(config), {}));
    for (unsigned frame = 0; frame < 4; ++frame)
    {
        CHECK(take(clear->execute()));
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
