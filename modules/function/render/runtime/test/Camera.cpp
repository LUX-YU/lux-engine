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

static void pixel(const lux::platform::ipc::IpcMessage& result, unsigned width, unsigned x, unsigned y,
    std::array<unsigned, 4> expected)
{
    CHECK(result.get<Readback::Response>().has_value());
    const auto bytes = result.get<RenderResult<ReadbackResult>>().value().pixels.bytes().view();
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
    auto server = take(createTestServer(host, config));
    const FeatureGraphOps graph_ops{describe, native_fixture::build};
    const auto feature = take(feature_test::registerGraph(*server,
        {&native_fixture::definition, &graph_ops, programs}));
    bool run_ok = false;
    const auto directory_id = take(server->commandId<ListCommands>());
    std::thread thread([&] { run_ok = bool(server->run()); });
    auto client = take(runtime_test::connect(config.channel_name, directory_id));
    CHECK(runtime_test::refresh(*client));
    const auto create_scene = take(runtime_test::find<CreateScene>(*client));
    const auto create_view = take(runtime_test::find<CreateView>(*client));
    const auto create_target = take(runtime_test::find<CreateTarget>(*client));
    const auto create_effect = take(runtime_test::find<CreateEffect>(*client));
    const auto attach = take(runtime_test::find<Attach>(*client));
    const auto update = take(runtime_test::find<UpdateCamera>(*client));
    const auto resize = take(runtime_test::find<ResizeTarget>(*client));
    const auto read = take(runtime_test::find<Readback>(*client));
    const auto progress = take(runtime_test::find<QueryExecutionProgress>(*client));
    const auto remove = take(runtime_test::find<RemoveScene>(*client));
    const auto scene = call<CreateScene>(*client, create_scene, {}).get<CreateScene::Response>()->scene;
    native_fixture::Config graph{32, 32, 1, 0};
    const auto target = native_fixture::target(graph);
    CreateEffectRequest effect_request{target, {{feature, "main", take(ConfigValue::make(graph))}}};
    auto created = call<CreateEffect>(*client, create_effect, effect_request);
    CHECK(created.get<CreateEffect::Response>().has_value());
    const auto effect = created.get<CreateEffect::Response>()->effect;
    auto a = camera_fixture::oracle({1, 0, 0, 1});
    auto b = camera_fixture::oracle({0, 1, 0, 1});
    const auto va = call<CreateView>(*client, create_view, {scene, a}).get<CreateView::Response>()->view;
    const auto vb = call<CreateView>(*client, create_view, {scene, b}).get<CreateView::Response>()->view;
    const auto ta = call<CreateTarget>(*client, create_target, {target}).get<CreateTarget::Response>()->target;
    const auto tb = call<CreateTarget>(*client, create_target, {target}).get<CreateTarget::Response>()->target;
    const AttachmentRequest ga{scene, va, ta, effect}, gb{scene, vb, tb, effect};
    CHECK(call<Attach>(*client, attach, ga).get<Attach::Response>().has_value());
    CHECK(call<Attach>(*client, attach, gb).get<Attach::Response>().has_value());
    pixel(call<Readback>(*client, read, {ga, {}}), 32, 16, 16, {255, 0, 0, 255});
    pixel(call<Readback>(*client, read, {gb, {}}), 32, 16, 16, {0, 255, 0, 255});
    const auto output = featureKey(native_fixture::type, "main", "pixels");
    pixel(call<Readback>(*client, read, {ga, output}), 32, 16, 16, {255, 0, 0, 255});
    auto missing = call<Readback>(*client, read, {ga, "missing.output"});
    CHECK(!missing.get<Readback::Response>() && missing.get<Readback::Response>().error().type == kRuntimeIdentity);
    auto image = call<Readback>(*client, read, {ga, "shared:color"});
    CHECK(!image.get<Readback::Response>() && image.get<Readback::Response>().error().type == vulkan::kUnsupported);
    pixel(call<Readback>(*client, read, {ga, output}), 32, 16, 16, {255, 0, 0, 255});
    const auto before = call<QueryExecutionProgress>(*client, progress, ga).get<QueryExecutionProgress::Response>();
    CHECK(before->count(EExecutionOutcome::EXECUTED) > 0);
    auto wrong_group = ga;
    wrong_group.view = {};
    auto wrong_progress = call<QueryExecutionProgress>(*client, progress, wrong_group);
    CHECK(!wrong_progress.get<QueryExecutionProgress::Response>() &&
        wrong_progress.get<QueryExecutionProgress::Response>().error().type == kRuntimeIdentity);
    const auto after = call<QueryExecutionProgress>(*client, progress, ga).get<QueryExecutionProgress::Response>();
    for (std::size_t i = 0; i < before->counts.size(); ++i) CHECK(after->counts[i] >= before->counts[i]);
    CHECK(call<QueryExecutionProgress>(*client, progress, gb).get<QueryExecutionProgress::Response>()->count(EExecutionOutcome::EXECUTED) > 0);
    const auto compiled = server->stats().compiled_graphs;
    b.projection[0] = .5f;
    b.viewport = {.5f, 0, .5f, 1};
    CHECK(call<UpdateCamera>(*client, update, {scene, vb, b}).get<UpdateCamera::Response>().has_value());
    // Camera Ack commits CPU state, not a displayed frame. Observe a subsequent actual output.
    const auto deadline = Clock::now() + 5s;
    for (;;)
    {
        auto frame = call<Readback>(*client, read, {gb, {}});
        CHECK(frame.get<Readback::Response>().has_value());
        const auto pixels = frame.get<Readback::Response>()->pixels.bytes().view();
        const auto green = std::to_integer<int>(pixels[(16 * 32 + 24) * 4 + 1]);
        if (std::abs(green - 128) <= 1) break;
        CHECK(Clock::now() < deadline);
    }
    pixel(call<Readback>(*client, read, {gb, {}}), 32, 24, 16, {0, 128, 0, 128});
    pixel(call<Readback>(*client, read, {gb, {}}), 32, 8, 16, {32, 64, 128, 255});
    pixel(call<Readback>(*client, read, {ga, {}}), 32, 16, 16, {255, 0, 0, 255});
    invalid = b; invalid.projection[15] = std::numeric_limits<float>::quiet_NaN();
    CHECK(call<UpdateCamera>(*client, update, {scene, vb, invalid}).get<UpdateCamera::Response>().has_value() == false);
    invalid = b; invalid.viewport.x = .75f;
    CHECK(call<UpdateCamera>(*client, update, {scene, vb, invalid}).get<UpdateCamera::Response>().has_value() == false);
    auto wrong = vb; ++wrong.gen;
    CHECK(call<UpdateCamera>(*client, update, {scene, wrong, b}).get<UpdateCamera::Response>().has_value() == false);
    pixel(call<Readback>(*client, read, {gb, {}}), 32, 24, 16, {0, 128, 0, 128});
    auto enlarged = target; enlarged.width = 64; enlarged.height = 48;
    CHECK(call<ResizeTarget>(*client, resize, {tb, enlarged}).get<ResizeTarget::Response>().has_value());
    pixel(call<Readback>(*client, read, {gb, {}}), 64, 48, 24, {0, 128, 0, 128});
    pixel(call<Readback>(*client, read, {gb, {}}), 64, 16, 24, {32, 64, 128, 255});
    graph.graphics = 2;
    effect_request.features[0].config = take(ConfigValue::make(graph));
    auto bad = call<CreateEffect>(*client, create_effect, std::move(effect_request));
    CHECK(!bad.get<CreateEffect::Response>() && bad.get<CreateEffect::Response>().error().type == kFeatureContract);
    CHECK(rejected_programs == 1);
    CHECK(server->stats().compiled_graphs == compiled);
    pixel(call<Readback>(*client, read, {ga, {}}), 32, 16, 16, {255, 0, 0, 255});
    CHECK(call<RemoveScene>(*client, remove, {scene}).get<RemoveScene::Response>().has_value());
    CHECK(call<UpdateCamera>(*client, update, {scene, vb, b}).get<UpdateCamera::Response>().has_value() == false);
    server->stop(); thread.join();
    CHECK(run_ok && server->stats().validation_errors == 0);
    std::puts("Camera complete matrices, isolated View/viewport, resize, stale/invalid rollback: PASS");
}
