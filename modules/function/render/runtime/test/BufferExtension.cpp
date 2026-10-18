#include "Discovery.hpp"
#include "FeatureFixture.hpp"
#include "BufferExtension.hpp"
#include "TypedTest.hpp"
#include "ServerFixture.hpp"
#include "TextureFixture.hpp"
#include "TextureSceneFixture.hpp"
#include "Allocation.hpp"
#include <cstring>

using namespace std::chrono_literals;

static void pixels(const lux::platform::ipc::IpcMessage& completion, unsigned expected)
{
    CHECK(bool(completion.get<Readback::Response>()));
    const auto bytes = completion.get<Readback::Response>()->pixels.bytes().view();
    CHECK(bytes.size() == 1024);
    for (auto byte : bytes) CHECK(std::to_integer<unsigned>(byte) == expected);
}

static void publishedPixels(runtime_test::Peer& client, CommandId read,
    AttachmentId group, unsigned previous, unsigned expected)
{
    // Readback returns an already submitted frame, not an implicit render-after-update transaction.
    // A frame accepted before publication may still return the old value; future frames must converge.
    const auto deadline = Clock::now() + 5s;
    while (Clock::now() < deadline)
    {
        const auto completion = call<Readback>(client, read, {group, {}});
        const auto bytes = completion.get<Readback::Response>()->pixels.bytes().view();
        CHECK(bytes.size() == 1024);
        if (std::ranges::all_of(bytes, [&](auto byte) { return std::to_integer<unsigned>(byte) == expected; })) return;
        for (auto byte : bytes) CHECK(std::to_integer<unsigned>(byte) == previous);
    }
    CHECK(false);
}

int main()
{
    using namespace extension_test;
    CpuFixture compiler(2);
    RenderServerConfig config{"runtime.external.buffer", true};
    vulkan::DeviceOptions config_device{.dynamic_rendering = true};
    config_device.multiple_queues = true;
    config.uploads = TransferOptions{65536, 2, 131072};
    config_device.texture_descriptors = true;
    auto server = take(createTestServer(compiler, config, config_device));
    const auto catalog = take(server->commandId<ListCommands>());
    CHECK(server->registerStorage<lux::render::TextureStorage>());
    const auto buffer_type = take(server->registerStorage<TestBufferStorage>());
    CHECK(take(server->registerStorage<TestBufferStorage>()) == buffer_type);
    const auto buffer_feature = take(server->registerFeature<TestBufferFeature>());
    CHECK(take(server->registerFeature<TestBufferFeature>()) == buffer_feature);
    CHECK(BufferCommands::constructed == 1 && BufferCommands::destroyed == 0);
    CHECK(feature_test::registerGraph(*server, {&texture_fixture::definition, &texture_fixture::definition_graph,
        [](const void* wire, std::string_view name) noexcept -> RenderResult<ProgramContribution> {
            const auto* config = static_cast<const texture_fixture::Config*>(wire);
            return ProgramContribution{texture_fixture::programs(*config, name), texture_fixture::arguments(*config)};
        }}));
    bool run_ok = false;
    std::thread thread([&] { render_allocation::participate = true; run_ok = bool(server->run()); });
    auto main = take(runtime_test::connect(config.channel_name, catalog));
    auto upload = take(runtime_test::connect(config.channel_name, catalog));
    CHECK(runtime_test::refresh(*main));
    CHECK(runtime_test::refresh(*upload));
    const auto texture_storage = take(runtime_test::prepareStorage(*upload, kTextureStorageType.name(), "default"));
    const auto buffer_storage = take(runtime_test::prepareStorage(*upload, TestBufferStorage::description.type.name(), "values"));
    auto create_buffer = take(runtime_test::find<SetBuffer>(*upload));
    auto remove_buffer = take(runtime_test::find<RemoveBuffer>(*upload));
    auto read_buffer = take(runtime_test::find<ReadBuffer>(*upload));
    auto upload_texture = take(runtime_test::find<SetTexture>(*upload));
    std::atomic<unsigned> destroyed{0};
    auto asset = std::shared_ptr<const std::vector<std::byte>>(new std::vector<std::byte>(1024, std::byte{128}),
        [&](auto* value) { delete value; ++destroyed; });
    std::array image_source{take(RenderPayload::fromVector(asset, 0, asset->size()))};
    const auto texture = decoded<TexturePublished>(call<SetTexture>(*upload, upload_texture,
        SetTexture::Request{{texture_storage, {}}, 0, information(16, 1, false), image_source[0]}));
    auto make_source = [](float value) {
        const std::array values{value, value, value, value};
        auto bytes = std::make_shared<std::vector<std::byte>>(16);
        std::memcpy(bytes->data(), values.data(), 16); // asset GPU float data, never a Wire object ABI
        return std::array{take(RenderPayload::fromVector(std::shared_ptr<const std::vector<std::byte>>(bytes), 0, 16))};
    };
    auto source = make_source(0.5f);
    const auto buffer = decoded<BufferBinding>(call<SetBuffer>(*upload, create_buffer, SetBuffer::Request{{buffer_storage, {}}, source[0]}));
    std::optional late_read{call<ReadBuffer>(*upload, read_buffer, buffer)};
    CHECK(late_read->get<ReadBuffer::Response>()->pixels.bytes().view().size() == 16);
    for (unsigned i = 0; i < 4; ++i)
    {
        float value = 0;
        std::memcpy(&value, late_read->get<ReadBuffer::Response>()->pixels.bytes().view().data() + i * 4, 4);
        CHECK(value == 0.5f);
    }
    FeatureUse selected{take(feature_test::find(*main, feature_type.name())), "combined",
        take(ConfigValue::make(ViewConfig{texture.texture, buffer.id}))};
    auto create_effect = take(runtime_test::find<CreateEffect>(*main));
    const auto effect = decoded<EffectCreated>(call<CreateEffect>(*main, create_effect,
        CreateEffectRequest{target, {selected}})).effect;
    auto create_scene = take(runtime_test::find<CreateScene>(*main));
    auto create_view = take(runtime_test::find<CreateView>(*main));
    auto create_target = take(runtime_test::find<CreateTarget>(*main));
    auto attach = take(runtime_test::find<Attach>(*main));
    auto detach = take(runtime_test::find<Detach>(*main));
    auto read = take(runtime_test::find<Readback>(*main));
    std::array<AttachmentRequest, 4> requests;
    std::array<AttachmentId, 4> groups;
    for (unsigned s = 0; s < 2; ++s)
    {
        const auto scene = decoded<SceneCreated>(call<CreateScene>(*main, create_scene, EmptyRequest{})).scene;
        for (unsigned v = 0; v < 2; ++v)
        {
            const auto view = decoded<ViewCreated>(call<CreateView>(*main, create_view, CreateViewRequest{scene})).view;
            const auto output = decoded<TargetCreated>(call<CreateTarget>(*main, create_target, CreateTargetRequest{target})).target;
            requests[2 * s + v] = {scene, view, output, effect};
            auto linked = call<Attach>(*main, attach, requests[2 * s + v]);
            groups[2 * s + v] = linked.get<Attach::Response>()->attachment;
            CHECK(linked.get<Attach::Response>()->storages.size() == 2);
            for (const auto& binding : linked.get<Attach::Response>()->storages)
            {
                if (binding.resource_key == "shared:texture") CHECK(binding.storage == texture_storage);
                else
                {
                    CHECK(binding.resource_key == "shared:values");
                    CHECK(binding.storage == buffer_storage);
                }
            }
        }
    }
    const auto attachment_storage = take(runtime_test::prepareStorage(*main,
        TestBufferStorage::description.type.name(), "private", groups[0]));
    CHECK(attachment_storage != buffer_storage);
    CHECK(take(runtime_test::coldCall<FindStorage>(*main, {groups[0], buffer_type, "private"})) == attachment_storage);
    for (const auto& group : groups) pixels(call<Readback>(*main, read, {group, {}}), 64);
    const auto before = server->stats().submissions;
    render_allocation::calls = 0;
    render_allocation::window = true;
    std::this_thread::sleep_for(200ms);
    render_allocation::window = false;
    CHECK(render_allocation::calls == 0 && server->stats().submissions > before + 20);
    source = make_source(0.25f);
    CHECK(decoded<BufferBinding>(call<SetBuffer>(*upload, create_buffer, SetBuffer::Request{buffer.id, source[0]})).id == buffer.id);
    // Owner-safe-point publication updates every existing Context without detach/recompile.
    for (const auto& group : groups) publishedPixels(*main, read, group, 64, 32);
    call<Detach>(*main, detach, groups[0]);
    CHECK(!runtime_test::coldCall<RemoveStorage>(*main, attachment_storage));
    CHECK(!runtime_test::coldCall<FindStorage>(*main, {groups[0], buffer_type, "private"}));
    const auto detached = groups[0];
    groups[0] = call<Attach>(*main, attach, requests[0]).get<Attach::Response>()->attachment;
    CHECK(groups[0] != detached);
    CHECK(!runtime_test::coldCall<Detach>(*main, detached));
    CHECK(!runtime_test::coldCall<Readback>(*main, {detached, {}}));
    pixels(call<Readback>(*main, read, {groups[0], {}}), 32);
    call<RemoveBuffer>(*upload, remove_buffer, buffer);
    CHECK(wait(*upload, take(runtime_test::send<ReadBuffer>(*upload, read_buffer, buffer))).get<ReadBuffer::Response>().has_value() == false);
    CHECK(wait(*upload, take(runtime_test::send<SetBuffer>(*upload, create_buffer, SetBuffer::Request{buffer.id, source[0]}))).get<SetBuffer::Response>().has_value() == false);
    const auto recreated = decoded<BufferBinding>(call<SetBuffer>(*upload, create_buffer, SetBuffer::Request{{buffer_storage, {}}, source[0]}));
    CHECK(recreated.id.local.index == buffer.id.local.index && recreated.id.local.gen != buffer.id.local.gen);
    pixels(call<Readback>(*main, read, {groups[0], {}}), 32);
    pixels(call<Readback>(*main, read, {groups[1], {}}), 32);
    // A second real Feature consumes the SAME Root Texture independently of the Buffer Feature.
    texture_fixture::Config sample{texture.texture};
    FeatureUse image_selected{take(feature_test::find(*main, texture_fixture::type.name())), "image", take(ConfigValue::make(sample))};
    const auto image_effect = decoded<EffectCreated>(call<CreateEffect>(*main, create_effect,
        CreateEffectRequest{target, {image_selected}})).effect;
    call<Detach>(*main, detach, groups[3]);
    requests[3].effect = image_effect;
    groups[3] = call<Attach>(*main, attach, requests[3]).get<Attach::Response>()->attachment;
    pixels(call<Readback>(*main, read, {groups[3], {}}), 128);
    const auto stats = server->stats();
    CHECK(stats.validation_errors == 0);
    server->stop();
    thread.join();
    CHECK(run_ok && server->stats().validation_errors == 0);
    server.reset();
    CHECK(BufferCommands::constructed == 1 && BufferCommands::destroyed == 1);
    upload.reset();
    main.reset();
    CHECK(late_read->get<ReadBuffer::Response>()->pixels.bytes().view().size() == 16); // returned CPU result survives every Native/Server owner
    late_read.reset();
    image_source = {};
    asset.reset();
    CHECK(destroyed == 1);
    std::printf("external Storage typed registration + GPU Create/Update/Remove/Readback; two Features/two Scenes/four Views; new=0; validation=0\n");
}
