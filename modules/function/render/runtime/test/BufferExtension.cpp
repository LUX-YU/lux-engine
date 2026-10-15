#include "BufferExtension.hpp"
#include "CompileWorker.hpp"
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

template <class T> static T decoded(RenderCompletion result)
{
    CHECK(result.header.outcome == EOperationOutcome::SUCCESS);
    return take(decodeWireBytes<T>({*result.schema, result.bytes(), result.attachments()}, {65536, SIZE_MAX}));
}

template <class T, class R> static RenderCompletion call(RenderClient& client,
    const BoundOperation<T, R>& operation, const T& input, std::span<const RenderPayload> attachments = {})
{
    auto result = wait(client, take(client.submit(operation, input, attachments, 2048)));
    CHECK(result.header.outcome == EOperationOutcome::SUCCESS);
    return result;
}

static void pixels(const RenderCompletion& completion, unsigned expected)
{
    CHECK(completion.header.outcome == EOperationOutcome::SUCCESS);
    const auto reference = take(decodeWireBytes<ReadbackResult>(
        {*completion.schema, completion.bytes(), completion.attachments()}, {128, 2048}));
    const auto bytes = completion.attachments()[reference.pixels.attachment_index];
    CHECK(bytes.size() == 1024);
    for (auto byte : bytes) CHECK(std::to_integer<unsigned>(byte) == expected);
}

int main()
{
    using namespace extension_test;
    CompileWorker compiler(2);
    PrepareWorker worker;
    RenderServerConfig config{"runtime.external.buffer", true};
    config.transfer_queues = true;
    config.uploads = {65536, 2, 131072, &worker, PrepareWorker::enqueue, PrepareWorker::join};
    auto server = take(RenderServer::create(compiler.configure(config)));
    CHECK(server->registerStorage<lux::render::TextureStorage>());
    CHECK(server->registerStorage<TestBufferStorage>());
    CHECK(!server->registerStorage<TestBufferStorage>());
    CHECK(server->registerFeature<TestBufferFeature>());
    CHECK(server->registerFeature({&texture_fixture::definition,
        [](const WireView* wire, std::string_view name) noexcept -> RenderResult<std::vector<PassProgram>> {
            auto config = decodeWire<texture_fixture::Config>(*wire, texture_fixture::limits);
            if (!config) return lux::cxx::unexpected(config.error());
            return texture_fixture::programs(*config, name);
        }}));
    bool run_ok = false;
    std::thread thread([&] { render_allocation::participate = true; run_ok = bool(server->run()); });
    auto main = take(RenderClient::create(config.channel_name));
    auto upload = take(RenderClient::create(config.channel_name));
    CHECK(wait(*main, take(main->hello())).header.outcome == EOperationOutcome::SUCCESS);
    CHECK(wait(*upload, take(upload->hello())).header.outcome == EOperationOutcome::SUCCESS);
    auto create_buffer = take(upload->bind<UploadBuffer, BufferBinding>(upload_operation));
    auto remove_buffer = take(upload->bind<BufferBinding>(remove_operation));
    auto read_buffer = take(upload->bind<BufferBinding, ReadbackResult>(read_operation));
    auto upload_texture = take(upload->bind<UploadTexture, TexturePublished>(uploadTextureOperation()));
    std::atomic<unsigned> destroyed{0};
    auto asset = std::shared_ptr<const std::vector<std::byte>>(new std::vector<std::byte>(1024, std::byte{128}),
        [&](auto* value) { delete value; ++destroyed; });
    std::array image_source{take(RenderPayload::fromVector(asset, 0, asset->size()))};
    const auto texture = decoded<TexturePublished>(call(*upload, upload_texture,
        UploadTexture{{}, 0, information(16, 1, false), {0, 0, 1024}}, image_source));
    auto make_source = [](float value) {
        const std::array values{value, value, value, value};
        auto bytes = std::make_shared<std::vector<std::byte>>(16);
        std::memcpy(bytes->data(), values.data(), 16); // asset GPU float data, never a Wire object ABI
        return std::array{take(RenderPayload::fromVector(std::shared_ptr<const std::vector<std::byte>>(bytes), 0, 16))};
    };
    auto source = make_source(0.5f);
    const auto buffer = decoded<BufferBinding>(call(*upload, create_buffer, UploadBuffer{{}, {0, 0, 16}}, source));
    auto late_read = call(*upload, read_buffer, buffer);
    CHECK(late_read.attachments()[0].size() == 16);
    for (unsigned i = 0; i < 4; ++i)
    {
        float value = 0;
        std::memcpy(&value, late_read.attachments()[0].data() + i * 4, 4);
        CHECK(value == 0.5f);
    }
    FeatureSelection selected{std::string(feature_type.name()), "combined", {1, 0}, &DataSchema<ViewConfig>::schema,
        take(encodeWire(ViewConfig{texture.texture, buffer.id}, {}, limits))};
    auto create_effect = take(main->bind<CreateEffectRequest, EffectCreated>(operations::createEffect()));
    const auto effect = decoded<EffectCreated>(call(*main, create_effect,
        CreateEffectRequest{target, take(encodeFeatureSelections({&selected, 1}))})).effect;
    auto create_scene = take(main->bind<EmptyRequest, SceneCreated>(operations::createScene()));
    auto create_view = take(main->bind<CreateViewRequest, ViewCreated>(operations::createView()));
    auto create_target = take(main->bind<CreateTargetRequest, TargetCreated>(operations::createTarget()));
    auto attach = take(main->bind<AttachmentRequest>(operations::attach()));
    auto detach = take(main->bind<AttachmentRequest>(operations::detach()));
    auto read = take(main->bind<AttachmentRequest, ReadbackResult>(operations::readback()));
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
    CHECK(decoded<BufferBinding>(call(*upload, create_buffer, UploadBuffer{buffer.id, {0, 0, 16}}, source)).id == buffer.id);
    // Future users keep their original valid backing until the explicit detach/attach boundary.
    pixels(call(*main, read, groups[0]), 64);
    call(*main, detach, groups[0]);
    call(*main, attach, groups[0]);
    pixels(call(*main, read, groups[0]), 32);
    call(*upload, remove_buffer, buffer);
    CHECK(wait(*upload, take(upload->submit(read_buffer, buffer, {}, 128))).header.outcome == EOperationOutcome::FAILED);
    CHECK(wait(*upload, take(upload->submit(create_buffer, UploadBuffer{buffer.id, {0, 0, 16}}, source))).header.outcome == EOperationOutcome::FAILED);
    const auto recreated = decoded<BufferBinding>(call(*upload, create_buffer, UploadBuffer{{}, {0, 0, 16}}, source));
    CHECK(recreated.id.index == buffer.id.index && recreated.id.gen != buffer.id.gen);
    pixels(call(*main, read, groups[0]), 32);
    pixels(call(*main, read, groups[1]), 64);
    // A second real Feature consumes the SAME Root Texture independently of the Buffer Feature.
    texture_fixture::Config sample{texture.texture};
    FeatureSelection image_selected{std::string(texture_fixture::type.name()), "image", {1, 0},
        &DataSchema<texture_fixture::Config>::describe(), take(encodeWire(sample, {}, texture_fixture::limits))};
    const auto image_effect = decoded<EffectCreated>(call(*main, create_effect,
        CreateEffectRequest{target, take(encodeFeatureSelections({&image_selected, 1}))})).effect;
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
    CHECK(late_read.attachments()[0].size() == 16); // returned CPU result survives every Native/Server owner
    late_read = {};
    image_source = {};
    asset.reset();
    CHECK(destroyed == 1);
    std::printf("external Storage typed registration + GPU Create/Update/Remove/Readback; two Features/two Scenes/four Views; new=0; validation=0\n");
}
