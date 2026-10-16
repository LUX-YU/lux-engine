#include "BufferExtension.hpp"
#include "TypedTest.hpp"
#include "ServerFixture.hpp"
#include "TextureFixture.hpp"
#include "TextureSceneFixture.hpp"
#include "Allocation.hpp"
#include <cstring>

using namespace std::chrono_literals;

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
        std::this_thread::sleep_for(100us);
    }
    std::abort();
}

template <RenderCommand C> static TypedRenderCompletion<C> call(RenderClient& client,
    const BoundCommand<C>& operation, typename C::Request input)
{
    auto result = wait(client, take(client.submit(operation, input, 2048)));
    CHECK(result.header.outcome == ECommandOutcome::SUCCESS);
    return take(std::move(result).take(operation));
}

static void pixels(const TypedRenderCompletion<Readback>& completion, unsigned expected)
{
    CHECK(completion.header().outcome == ECommandOutcome::SUCCESS);
    const auto bytes = completion.response().pixels.bytes().view();
    CHECK(bytes.size() == 1024);
    for (auto byte : bytes) CHECK(std::to_integer<unsigned>(byte) == expected);
}

int main()
{
    using namespace extension_test;
    CpuFixture compiler(2);
    RenderServerConfig config{"runtime.external.buffer", true};
    config.transfer_queues = true;
    config.uploads = TransferOptions{65536, 2, 131072};
    auto server = take(RenderServer::create(withCpu(compiler, config)));
    CHECK(server->registerStorage<lux::render::TextureStorage>());
    CHECK(server->registerStorage<TestBufferStorage>());
    CHECK(!server->registerStorage<TestBufferStorage>());
    CHECK(server->registerFeature<TestBufferFeature>());
    CHECK(server->registerFeature({&texture_fixture::definition, &texture_fixture::definition_graph,
        [](const WireView* wire, std::string_view name) noexcept -> RenderResult<ProgramContribution> {
            auto config = decodeWire<texture_fixture::Config>(*wire, texture_fixture::limits);
            if (!config) return lux::cxx::unexpected(config.error());
            return ProgramContribution{texture_fixture::programs(*config, name), texture_fixture::arguments(*config)};
        }}));
    bool run_ok = false;
    std::thread thread([&] { render_allocation::participate = true; run_ok = bool(server->run()); });
    auto main = take(RenderClient::create(config.channel_name));
    auto upload = take(RenderClient::create(config.channel_name));
    CHECK(wait(*main, take(main->hello())).header.outcome == ECommandOutcome::SUCCESS);
    CHECK(wait(*upload, take(upload->hello())).header.outcome == ECommandOutcome::SUCCESS);
    auto create_buffer = take(upload->bind<SetBuffer>());
    auto remove_buffer = take(upload->bind<RemoveBuffer>());
    auto read_buffer = take(upload->bind<ReadBuffer>());
    auto upload_texture = take(upload->bind<SetTexture>());
    std::atomic<unsigned> destroyed{0};
    auto asset = std::shared_ptr<const std::vector<std::byte>>(new std::vector<std::byte>(1024, std::byte{128}),
        [&](auto* value) { delete value; ++destroyed; });
    std::array image_source{take(RenderPayload::fromVector(asset, 0, asset->size()))};
    const auto texture = decoded<TexturePublished>(call(*upload, upload_texture,
        SetTexture::Request{{}, 0, information(16, 1, false), image_source[0]}));
    auto make_source = [](float value) {
        const std::array values{value, value, value, value};
        auto bytes = std::make_shared<std::vector<std::byte>>(16);
        std::memcpy(bytes->data(), values.data(), 16); // asset GPU float data, never a Wire object ABI
        return std::array{take(RenderPayload::fromVector(std::shared_ptr<const std::vector<std::byte>>(bytes), 0, 16))};
    };
    auto source = make_source(0.5f);
    const auto buffer = decoded<BufferBinding>(call(*upload, create_buffer, SetBuffer::Request{{}, source[0]}));
    std::optional late_read{call(*upload, read_buffer, buffer)};
    CHECK(late_read->response().pixels.bytes().view().size() == 16);
    for (unsigned i = 0; i < 4; ++i)
    {
        float value = 0;
        std::memcpy(&value, late_read->response().pixels.bytes().view().data() + i * 4, 4);
        CHECK(value == 0.5f);
    }
    FeatureSelection selected{std::string(feature_type.name()), "combined", {1, 0}, DataSchema<ViewConfig>::schema,
        RenderPayload::copyOf(take(encodeWire(ViewConfig{texture.texture, buffer.id}, {}, limits)).view())};
    auto create_effect = take(main->bind<CreateEffect>());
    const auto effect = decoded<EffectCreated>(call(*main, create_effect,
        CreateEffectRequest{target, {selected}})).effect;
    auto create_scene = take(main->bind<CreateScene>());
    auto create_view = take(main->bind<CreateView>());
    auto create_target = take(main->bind<CreateTarget>());
    auto attach = take(main->bind<Attach>());
    auto detach = take(main->bind<Detach>());
    auto read = take(main->bind<Readback>());
    std::array<AttachmentRequest, 4> groups;
    for (unsigned s = 0; s < 2; ++s)
    {
        const auto scene = decoded<SceneCreated>(call(*main, create_scene, EmptyRequest{})).scene;
        for (unsigned v = 0; v < 2; ++v)
        {
            const auto view = decoded<ViewCreated>(call(*main, create_view, CreateViewRequest{scene})).view;
            const auto output = decoded<TargetCreated>(call(*main, create_target, CreateTargetRequest{target})).target;
            groups[2 * s + v] = {scene, view, output, effect};
            call(*main, attach, groups[2 * s + v]);
        }
    }
    for (const auto& group : groups) pixels(call(*main, read, group), 64);
    const auto before = server->stats().submissions;
    render_allocation::calls = 0;
    render_allocation::window = true;
    std::this_thread::sleep_for(200ms);
    render_allocation::window = false;
    CHECK(render_allocation::calls == 0 && server->stats().submissions > before + 20);
    source = make_source(0.25f);
    CHECK(decoded<BufferBinding>(call(*upload, create_buffer, SetBuffer::Request{buffer.id, source[0]})).id == buffer.id);
    // Future users keep their original valid backing until the explicit detach/attach boundary.
    pixels(call(*main, read, groups[0]), 64);
    call(*main, detach, groups[0]);
    call(*main, attach, groups[0]);
    pixels(call(*main, read, groups[0]), 32);
    call(*upload, remove_buffer, buffer);
    CHECK(wait(*upload, take(submitValue(*upload, read_buffer, buffer, 128))).header.outcome == ECommandOutcome::FAILED);
    CHECK(wait(*upload, take(submitValue(*upload, create_buffer, SetBuffer::Request{buffer.id, source[0]}))).header.outcome == ECommandOutcome::FAILED);
    const auto recreated = decoded<BufferBinding>(call(*upload, create_buffer, SetBuffer::Request{{}, source[0]}));
    CHECK(recreated.id.index == buffer.id.index && recreated.id.gen != buffer.id.gen);
    pixels(call(*main, read, groups[0]), 32);
    pixels(call(*main, read, groups[1]), 64);
    // A second real Feature consumes the SAME Root Texture independently of the Buffer Feature.
    texture_fixture::Config sample{texture.texture};
    FeatureSelection image_selected{std::string(texture_fixture::type.name()), "image", {1, 0},
        DataSchema<texture_fixture::Config>::describe(), RenderPayload::copyOf(take(encodeWire(sample, {}, texture_fixture::limits)).view())};
    const auto image_effect = decoded<EffectCreated>(call(*main, create_effect,
        CreateEffectRequest{target, {image_selected}})).effect;
    call(*main, detach, groups[3]);
    groups[3].effect = image_effect;
    call(*main, attach, groups[3]);
    pixels(call(*main, read, groups[3]), 128);
    const auto stats = server->stats();
    CHECK(stats.validation_errors == 0);
    server->stop();
    thread.join();
    CHECK(run_ok && server->stats().validation_errors == 0);
    server.reset();
    upload.reset();
    main.reset();
    CHECK(late_read->response().pixels.bytes().view().size() == 16); // returned CPU result survives every Native/Server owner
    late_read.reset();
    image_source = {};
    asset.reset();
    CHECK(destroyed == 1);
    std::printf("external Storage typed registration + GPU Create/Update/Remove/Readback; two Features/two Scenes/four Views; new=0; validation=0\n");
}
