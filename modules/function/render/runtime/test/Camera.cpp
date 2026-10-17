#include "../../native/vulkan/test/Support.hpp"
#include "Discovery.hpp"
#include "ServerFixture.hpp"
#include "TypedTest.hpp"
#include <lux/engine/render/runtime/RenderServer.hpp>
#include "CameraFixture.hpp"
#include "FeatureFixture.hpp"
#include <limits>

using namespace foundation_test;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

static RenderCompletion wait(RenderClient& client, RequestId id)
{
    const auto deadline = Clock::now() + 15s;
    while (Clock::now() < deadline)
    {
        RenderCompletion result;
        if (take(client.tryReceive(result)) == ERenderReceive::RECEIVED)
        {
            CHECK(result.header.request == id);
            return result;
        }
        std::this_thread::yield();
    }
    std::abort();
}

template <RenderCommand C> static TypedRenderCompletion<C> call(
    RenderClient& client, const BoundCommand<C>& binding, typename C::Request input
)
{
    auto result = wait(client, take(client.submit(binding, input, 128 * 1024)));
    return take(std::move(result).take(binding));
}


static std::atomic<unsigned> rejected_programs{0};

static RenderResult<void> describe(const void* value, std::string_view name, ResourceClaims& claims) noexcept
{
    auto config = *static_cast<const native_fixture::Config*>(value);
    if (config.graphics == 2) config.graphics = 1; // Reach the actual Runtime parameter ABI boundary.
    return native_fixture::describe(&config, name, claims);
}

static RenderResult<ProgramContribution> programs(const void* value, std::string_view name) noexcept
{
    const auto config = *static_cast<const native_fixture::Config*>(value);
    if (config.graphics == 2) ++rejected_programs;
    return ProgramContribution{config.graphics == 2 ? native_fixture::programs(config, name) :
        camera_fixture::programs(config, name), {}};
}

static void pixel(const TypedRenderCompletion<Readback>& result, unsigned width, unsigned x, unsigned y,
    std::array<unsigned, 4> expected)
{
    CHECK(result.header().outcome == ECommandOutcome::SUCCESS);
    const auto bytes = result.response().pixels.bytes().view();
    CHECK((y * width + x + 1) * 4 <= bytes.size());
    for (unsigned channel = 0; channel < 4; ++channel)
        CHECK(std::abs(int(std::to_integer<unsigned>(bytes[(y * width + x) * 4 + channel])) -
            int(expected[channel])) <= 1);
}

int main()
{
    CameraState invalid;
    invalid.view[0] = std::numeric_limits<float>::infinity();
    CHECK(!invalid.validate());
    invalid = {};
    invalid.viewport.width = 2;
    CHECK(!invalid.validate());
    invalid = {};
    invalid.viewport.min_depth = .8f; invalid.viewport.max_depth = .2f;
    CHECK(!invalid.validate());
    CHECK(cameraParameters(CameraState{}).size() == 128);
    CpuFixture host;
    RenderServerConfig config{"runtime.camera", true};
    auto server = take(RenderServer::create(withCpu(host, config)));
    const FeatureGraphOps graph_ops{describe, native_fixture::build};
    const auto feature = take(feature_test::registerGraph(*server,
        {&native_fixture::definition, &graph_ops, programs}));
    bool run_ok = false;
    std::thread thread([&] { run_ok = bool(server->run()); });
    auto client = take(RenderClient::create(config.channel_name));
    CHECK(wait(*client, take(client->hello())).header.outcome == ECommandOutcome::SUCCESS);
    CHECK(runtime_test::refresh(*client));
    const auto create_scene = take(client->bind<CreateScene>());
    const auto create_view = take(client->bind<CreateView>());
    const auto create_target = take(client->bind<CreateTarget>());
    const auto create_effect = take(client->bind<CreateEffect>());
    const auto attach = take(client->bind<Attach>());
    const auto update = take(client->bind<UpdateCamera>());
    const auto resize = take(client->bind<ResizeTarget>());
    const auto read = take(client->bind<Readback>());
    const auto remove = take(client->bind<RemoveScene>());
    const auto scene = call(*client, create_scene, {}).response().scene;
    native_fixture::Config graph{32, 32, 1, 0};
    const auto target = native_fixture::target(graph);
    CreateEffectRequest effect_request{target, {{feature, "main", take(ConfigValue::make(graph))}}};
    auto created = call(*client, create_effect, effect_request);
    CHECK(created.header().outcome == ECommandOutcome::SUCCESS);
    const auto effect = created.response().effect;
    auto a = camera_fixture::oracle({1, 0, 0, 1});
    auto b = camera_fixture::oracle({0, 1, 0, 1});
    const auto va = call(*client, create_view, {scene, a}).response().view;
    const auto vb = call(*client, create_view, {scene, b}).response().view;
    const auto ta = call(*client, create_target, {target}).response().target;
    const auto tb = call(*client, create_target, {target}).response().target;
    const AttachmentRequest ga{scene, va, ta, effect}, gb{scene, vb, tb, effect};
    CHECK(call(*client, attach, ga).header().outcome == ECommandOutcome::SUCCESS);
    CHECK(call(*client, attach, gb).header().outcome == ECommandOutcome::SUCCESS);
    pixel(call(*client, read, ga), 32, 16, 16, {255, 0, 0, 255});
    pixel(call(*client, read, gb), 32, 16, 16, {0, 255, 0, 255});
    const auto compiled = server->stats().compiled_graphs;
    b.projection[0] = .5f;
    b.viewport = {.5f, 0, .5f, 1};
    CHECK(call(*client, update, {scene, vb, b}).header().outcome == ECommandOutcome::SUCCESS);
    // Camera Ack commits CPU state, not a displayed frame. Observe a subsequent actual output.
    const auto deadline = Clock::now() + 5s;
    for (;;)
    {
        auto frame = call(*client, read, gb);
        CHECK(frame.header().outcome == ECommandOutcome::SUCCESS);
        const auto pixels = frame.response().pixels.bytes().view();
        const auto green = std::to_integer<int>(pixels[(16 * 32 + 24) * 4 + 1]);
        if (std::abs(green - 128) <= 1) break;
        CHECK(Clock::now() < deadline);
    }
    pixel(call(*client, read, gb), 32, 24, 16, {0, 128, 0, 128});
    pixel(call(*client, read, gb), 32, 8, 16, {32, 64, 128, 255});
    pixel(call(*client, read, ga), 32, 16, 16, {255, 0, 0, 255});
    invalid = b; invalid.projection[15] = std::numeric_limits<float>::quiet_NaN();
    CHECK(call(*client, update, {scene, vb, invalid}).header().outcome == ECommandOutcome::FAILED);
    invalid = b; invalid.viewport.x = .75f;
    CHECK(call(*client, update, {scene, vb, invalid}).header().outcome == ECommandOutcome::FAILED);
    auto wrong = vb; ++wrong.gen;
    CHECK(call(*client, update, {scene, wrong, b}).header().outcome == ECommandOutcome::FAILED);
    pixel(call(*client, read, gb), 32, 24, 16, {0, 128, 0, 128});
    auto enlarged = target; enlarged.width = 64; enlarged.height = 48;
    CHECK(call(*client, resize, {tb, enlarged}).header().outcome == ECommandOutcome::SUCCESS);
    pixel(call(*client, read, gb), 64, 48, 24, {0, 128, 0, 128});
    pixel(call(*client, read, gb), 64, 16, 24, {32, 64, 128, 255});
    graph.graphics = 2;
    effect_request.features[0].config = take(ConfigValue::make(graph));
    auto bad = call(*client, create_effect, std::move(effect_request));
    CHECK(bad.header().outcome == ECommandOutcome::FAILED && bad.header().error.type == kFeatureContract);
    CHECK(rejected_programs == 1);
    CHECK(server->stats().compiled_graphs == compiled);
    pixel(call(*client, read, ga), 32, 16, 16, {255, 0, 0, 255});
    CHECK(call(*client, remove, {scene}).header().outcome == ECommandOutcome::SUCCESS);
    CHECK(call(*client, update, {scene, vb, b}).header().outcome == ECommandOutcome::FAILED);
    server->stop(); thread.join();
    CHECK(run_ok && server->stats().validation_errors == 0);
    std::puts("Camera complete matrices, isolated View/viewport, resize, stale/invalid rollback: PASS");
}
