#include "Discovery.hpp"
#include "FeatureFixture.hpp"
#include "ServerFixture.hpp"
#include "TypedTest.hpp"
#include "TextureFixture.hpp"
#include "TextureSceneFixture.hpp"
#include "Allocation.hpp"
#include <lux/engine/render/runtime/Commands.hpp>
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/render/resource/Texture.hpp>
#include <fstream>
#include <latch>

using namespace std::chrono_literals;
static std::latch* compile_entered = nullptr;
static std::latch* compile_resume = nullptr;
static std::atomic<bool> block_compile{false};
static std::atomic<unsigned> measure_phase{0};
struct Cadence final { unsigned phase; std::int64_t timestamp_ns; };
static std::array<Cadence, 16384> cadence{};
static std::size_t cadence_count = 0;

namespace lux::render::vulkan::test
{
    VkResult before(const char*) noexcept { return VK_SUCCESS; }

    VkResult after(const char* name, VkResult result) noexcept
    {
        if (std::string_view(name) == "cmd_draw")
        {
            static unsigned draws = 0, previous_phase = 0;
            const auto phase = measure_phase.load();
            if (phase != previous_phase) { draws = 0; previous_phase = phase; }
            if (phase && ++draws % 4 == 0)
            {
                CHECK(cadence_count < cadence.size());
                cadence[cadence_count++] = {phase,
                    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count()};
            }
        }
        return result;
    }

    void destroyed(const char*) noexcept {}
}

static RenderResult<ProgramContribution> programs(const void* wire, std::string_view name) noexcept
{
    if (block_compile.load())
    {
        compile_entered->count_down();
        compile_resume->wait();
    }
    const auto* config = static_cast<const texture_fixture::Config*>(wire);
    return ProgramContribution{texture_fixture::programs(*config, name), texture_fixture::arguments(*config)};
}

static void pixels(const lux::platform::ipc::IpcMessage& value, unsigned expected)
{
    CHECK(value.get<Readback::Response>().has_value());
    const auto bytes = value.get<Readback::Response>()->pixels.bytes().view();
    CHECK(bytes.size() == 16 * 16 * 4);
    for (auto b : bytes) CHECK(std::to_integer<unsigned>(b) == expected);
}

int main(int argc, char** argv)
{
    // Preserve the former two compiler workers plus one upload worker on the shared host scheduler.
    CpuFixture compiler(3);
    RenderServerConfig config{"runtime.texture.rendering", true};
    vulkan::DeviceOptions config_device{.dynamic_rendering = true};
    config_device.multiple_queues = argc == 1;
    config.uploads = TransferOptions{1024 * 1024, 2, 128 * 1024 * 1024, true};
    config_device.texture_descriptors = true;
    auto server = take(createTestServer(compiler, config, config_device));
    const auto catalog = take(server->commandId<ListCommands>());
    CHECK(server->registerStorage<lux::render::TextureStorage>());
    CHECK(feature_test::registerGraph(*server, {&texture_fixture::definition, &texture_fixture::definition_graph, programs}));
    bool run_ok = false;
    std::thread thread([&] { render_allocation::participate = true; run_ok = bool(server->run()); });
    auto a = take(runtime_test::connect(config.channel_name, catalog));
    auto b = take(runtime_test::connect(config.channel_name, catalog));
    CHECK(runtime_test::refresh(*a));
    CHECK(runtime_test::refresh(*b));
    const auto storage = take(runtime_test::prepareStorage(*b, kTextureStorageType.name(), "default"));
    auto upload = take(runtime_test::find<SetTexture>(*b));
    auto release = take(runtime_test::find<ReleaseTexture>(*b));
    std::atomic<unsigned> destroyed{0};
    auto asset = std::shared_ptr<const std::vector<std::byte>>(new std::vector<std::byte>(16 * 16 * 4, std::byte{31}),
        [&](auto* bytes) { delete bytes; ++destroyed; });
    std::array payload{take(RenderPayload::fromVector(asset, 0, asset->size()))};
    auto info = information(16, 1, false);
    SetTexture::Request input{{storage, {}}, 0, info, payload[0]};
    auto texture = decoded<TexturePublished>(call<SetTexture>(*b, upload, input));
    texture_fixture::Config sample{texture.texture};
    FeatureUse selected{take(feature_test::find(*a, texture_fixture::type.name())), "main", take(ConfigValue::make(sample))};
    const CreateEffectRequest effect_input{texture_fixture::target(sample), {selected}};
    auto create_effect_a = take(runtime_test::find<CreateEffect>(*a));
    auto create_effect_b = take(runtime_test::find<CreateEffect>(*b));
    const auto effect = decoded<EffectCreated>(call<CreateEffect>(*a, create_effect_a, effect_input)).effect;
    auto create_scene = take(runtime_test::find<CreateScene>(*a));
    auto create_view = take(runtime_test::find<CreateView>(*a));
    auto create_target = take(runtime_test::find<CreateTarget>(*a));
    auto attach = take(runtime_test::find<Attach>(*a));
    auto read = take(runtime_test::find<Readback>(*a));
    std::array<AttachmentId, 4> attachments;
    for (unsigned s = 0; s < 2; ++s)
    {
        const auto scene = decoded<SceneCreated>(call<CreateScene>(*a, create_scene, EmptyRequest{})).scene;
        for (unsigned v = 0; v < 2; ++v)
        {
            const auto view = decoded<ViewCreated>(call<CreateView>(*a, create_view, CreateViewRequest{scene})).view;
            const auto target = decoded<TargetCreated>(call<CreateTarget>(*a, create_target,
                CreateTargetRequest{texture_fixture::target(sample)})).target;
            attachments[2 * s + v] = call<Attach>(*a, attach, {scene, view, target, effect})
                .get<Attach::Response>()->attachment;
        }
    }
    for (const auto& attachment : attachments) pixels(call<Readback>(*a, read, {attachment, {}}), 31);
    const auto before = server->stats();
    render_allocation::calls = 0;
    render_allocation::window = true;
    measure_phase = 1;
    std::this_thread::sleep_for(300ms);
    measure_phase = 0;
    render_allocation::window = false;
    CHECK(render_allocation::calls == 0 && server->stats().submissions > before.submissions + 40);

    // Two real Shader/Pipeline compiler workers overlap Upload and four prepared consumers.
    {
        std::latch entered(2), resume(1);
        compile_entered = &entered; compile_resume = &resume;
        block_compile = true;
        const auto ca = take(runtime_test::send<CreateEffect>(*a, create_effect_a, effect_input));
        const auto cb = take(runtime_test::send<CreateEffect>(*b, create_effect_b, effect_input));
        entered.wait();
        auto replacement = std::make_shared<const std::vector<std::byte>>(asset->size(), std::byte{64});
        std::array owner{take(RenderPayload::fromVector(replacement, 0, replacement->size()))};
        input.texture = texture.texture; input.expected_revision = texture.revision;
        input.source = owner[0];
        texture = decoded<TexturePublished>(call<SetTexture>(*b, upload, input));
        block_compile = false; resume.count_down();
        CHECK(wait(*a, ca).get<CreateEffect::Response>().has_value());
        CHECK(wait(*b, cb).get<CreateEffect::Response>().has_value());
    }
    for (const auto& attachment : attachments) pixels(call<Readback>(*a, read, {attachment, {}}), 64);

    std::array<RequestId, 64> paused;
    for (auto& id : paused) id = take(runtime_test::send<CreateScene>(*a, create_scene, EmptyRequest{}));
    const auto additional = take(runtime_test::send<CreateScene>(*a, create_scene, EmptyRequest{}));
    auto large = std::make_shared<const std::vector<std::byte>>(16 * 1024 * 1024, std::byte{91});
    std::array large_owner{take(RenderPayload::fromVector(large, 0, large->size()))};
    const SetTexture::Request large_input{{storage, {}}, 0, information(2048, 1, false), large_owner[0]};
    const auto stalled = server->stats();
    const auto until = Clock::now() + 1500ms;
    std::ofstream csv(config_device.multiple_queues ? "texture-rendering-dedicated.csv" : "texture-rendering-alias.csv");
    csv << "sample,bytes,submit_move_ns,completion_consume_type_access_ns,total_ns,graphics_submissions\n";
    unsigned progress = 0;
    measure_phase = 2;
    while (Clock::now() < until)
    {
        const auto start = Clock::now();
        const auto id = take(runtime_test::send<SetTexture>(*b, upload, large_input));
        const auto submitted = Clock::now();
        auto result = wait(*b, id);
        const auto consumed = Clock::now();
        const auto value = decoded<TexturePublished>(std::move(result));
        const auto decoded_at = Clock::now();
        csv << progress << ',' << large->size() << ','
            << std::chrono::duration_cast<std::chrono::nanoseconds>(submitted - start).count() << ','
            << std::chrono::duration_cast<std::chrono::nanoseconds>(decoded_at - consumed).count() << ','
            << std::chrono::duration_cast<std::chrono::nanoseconds>(decoded_at - start).count() << ','
            << server->stats().submissions - stalled.submissions << '\n';
        call<ReleaseTexture>(*b, release, value);
        ++progress;
    }
    measure_phase = 0;
    CHECK(progress >= 2 && server->stats().submissions > stalled.submissions + 40);
    for (auto id : paused) CHECK(wait(*a, id).get<CreateScene::Response>().has_value());
    CHECK(wait(*a, additional).get<CreateScene::Response>().has_value());
    b.reset(); // creation Client and its transient Owner are independent of Global Texture.
    asset.reset(); payload = {}; input.source = {};
    CHECK(destroyed == 1);
    for (const auto& attachment : attachments) pixels(call<Readback>(*a, read, {attachment, {}}), 64);
    const auto stats = server->stats();
    server->stop(); thread.join();
    std::ofstream frames(config_device.multiple_queues ? "texture-cadence-dedicated.csv" : "texture-cadence-alias.csv");
    frames << "sample,phase,recorded_four_view_interval_ns\n";
    for (std::size_t i = 1; i < cadence_count; ++i)
        if (cadence[i].phase == cadence[i - 1].phase)
            frames << i << ',' << cadence[i].phase << ',' << cadence[i].timestamp_ns - cadence[i - 1].timestamp_ns << '\n';
    CHECK(run_ok && server->stats().validation_errors == 0);
    std::printf("texture rendering queue=%s 2scene/4view new=0 two_compile_workers=2 "
        "main64_full_ms=1500 upload_progress=%u graphics_progress=%llu compiled_graphs=%llu warnings=%llu\n",
        config_device.multiple_queues ? "dedicated" : "alias", progress, stats.submissions - stalled.submissions,
        stats.compiled_graphs, server->stats().validation_warnings);
}
