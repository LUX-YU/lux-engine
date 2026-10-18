#include "Discovery.hpp"
#include "ServerFixture.hpp"
#include "TypedTest.hpp"
#include "TextureFixture.hpp"
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/render/resource/Texture.hpp>

using namespace std::chrono_literals;

// Success establishes device dependencies; final staging pages may still await completion.
// Retry only the explicit capacity boundary, preserving the previous public revision and source.
template <RenderCommand C>
TexturePublished upload(runtime_test::Peer& peer, CommandId command, const typename C::Request& input)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    const auto* source = input.source.bytes().data();
    for (;;)
    {
        auto message = call<C>(peer, command, input);
        const auto& result = message.get<typename C::Response>();
        if (result)
        {
            return *result;
        }
        CHECK(result.error().type == vulkan::kCapacity);
        CHECK(std::chrono::steady_clock::now() < deadline);
        CHECK(input.source.bytes().data() == source);
        if (!input.texture.isNull())
        {
            const auto query = take(runtime_test::find<QueryTexture>(peer));
            const auto current = decoded<TexturePublished>(call<QueryTexture>(peer, query, input.texture));
            CHECK(current.revision == input.expected_revision);
        }
    }
}

int main()
{
    CpuFixture host;
    RenderServerConfig config{"runtime.texture.operations", true};
    vulkan::DeviceOptions config_device{.dynamic_rendering = true};
    config_device.multiple_queues = true;
    config.uploads = TransferOptions{1024 * 1024, 2, 128 * 1024 * 1024};
    config_device.texture_descriptors = true;
    auto server = take(createTestServer(host, config, config_device));
    const auto catalog = take(server->commandId<ListCommands>());
    CHECK(server->registerStorage<lux::render::TextureStorage>({16, 256 * 1024 * 1024}));
    bool run_ok = false;
    std::thread thread([&] { run_ok = bool(server->run()); });
    auto a = take(runtime_test::connect(config.channel_name, catalog));
    auto b = take(runtime_test::connect(config.channel_name, catalog));
    CHECK(runtime_test::refresh(*a));
    CHECK(runtime_test::refresh(*b));
    const auto storage = take(runtime_test::prepareStorage(*a, kTextureStorageType.name(), "default"));
    CHECK(take(runtime_test::prepareStorage(*b, kTextureStorageType.name(), "default")) == storage);
    auto bound_a = take(runtime_test::find<SetTexture>(*a));
    auto bound_b = take(runtime_test::find<SetTexture>(*b));
    auto info = information(64, 4, false);
    const auto last = info.mip_ranges[3];
    std::atomic<unsigned> destroyed{0};
    auto asset = std::shared_ptr<const std::vector<std::byte>>(
        new std::vector<std::byte>(last.offset + last.size, std::byte{31}),
        [&](auto* value) { delete value; ++destroyed; });
    std::array payload{take(RenderPayload::fromVector(asset, 0, asset->size()))};
    SetTexture::Request input{{storage, {}}, 0, info, payload[0]};
    // Typed messages cannot express an out-of-range external attachment index. An empty source
    // remains a real native upload boundary error, and both clients recycle the failed request.
    auto malformed = input;
    malformed.source = {};
    CHECK(wait(*a, take(runtime_test::send<SetTexture>(*a, bound_a, malformed))).get<SetTexture::Response>().has_value() == false);
    CHECK(wait(*b, take(runtime_test::send<SetTexture>(*b, bound_b, malformed))).get<SetTexture::Response>().has_value() == false);
    malformed = input;
    malformed.info.mip_ranges[0].size = UINT64_MAX;
    CHECK(wait(*a, take(runtime_test::send<SetTexture>(*a, bound_a, malformed))).get<SetTexture::Response>().has_value() == false);
    CHECK(wait(*b, take(runtime_test::send<SetTexture>(*b, bound_b, malformed))).get<SetTexture::Response>().has_value() == false);
    // Same numeric RequestId on distinct physical clients never returns the other TextureId.
    auto id_a = take(runtime_test::send<SetTexture>(*a, bound_a, input));
    auto id_b = take(runtime_test::send<SetTexture>(*b, bound_b, input));
    CHECK(id_a == id_b);
    auto texture_a = decoded<TexturePublished>(wait(*a, id_a));
    auto texture_b = decoded<TexturePublished>(wait(*b, id_b));
    CHECK(texture_a.texture != texture_b.texture && texture_a.revision == 1 && texture_b.revision == 1);
    a.reset(); // creator is not the Global storage owner
    input.texture = texture_a.texture;
    input.expected_revision = 1;
    texture_a = upload<SetTexture>(*b, bound_b, input);
    CHECK(texture_a.revision == 2);
    CHECK(wait(*b, take(runtime_test::send<SetTexture>(*b, bound_b, input))).get<SetTexture::Response>().has_value() == false);
    auto region = take(runtime_test::find<SetTextureRegion>(*b));
    auto patch = std::make_shared<const std::vector<std::byte>>(28, std::byte{92});
    std::array patch_payload{take(RenderPayload::fromVector(patch, 0, patch->size()))};
    texture_a = upload<SetTextureRegion>(
        *b, region, {texture_a.texture, 2, {1, 2, 1, 3, 2, 16}, patch_payload[0]}
    );
    CHECK(texture_a.revision == 3);
    auto query = take(runtime_test::find<QueryTexture>(*b));
    const auto current = decoded<TexturePublished>(wait(*b, take(runtime_test::send<QueryTexture>(*b, query, texture_a.texture))));
    CHECK(current.texture == texture_a.texture && current.revision == texture_a.revision);
    CHECK(wait(*b, take(runtime_test::send<QueryTexture>(*b, query, TextureId{}))).get<QueryTexture::Response>().has_value() == false);
    auto release = take(runtime_test::find<ReleaseTexture>(*b));
    CHECK(wait(*b, take(runtime_test::send<ReleaseTexture>(*b, release, texture_a))).get<ReleaseTexture::Response>().has_value());
    CHECK(wait(*b, take(runtime_test::send<QueryTexture>(*b, query, texture_a.texture))).get<QueryTexture::Response>().has_value() == false);
    CHECK(wait(*b, take(runtime_test::send<ReleaseTexture>(*b, release, texture_a))).get<ReleaseTexture::Response>().has_value() == false);
    CHECK(wait(*b, take(runtime_test::send<ReleaseTexture>(*b, release, texture_b))).get<ReleaseTexture::Response>().has_value());
    input.texture = {storage, {}}; input.expected_revision = 0;
    const auto replacement = upload<SetTexture>(*b, bound_b, input);
    CHECK(replacement.texture != texture_a.texture && replacement.texture != texture_b.texture);
    CHECK(wait(*b, take(runtime_test::send<ReleaseTexture>(*b, release, replacement))).get<ReleaseTexture::Response>().has_value());
    asset.reset(); payload = {}; input.source = {}; malformed.source = {};
    CHECK(destroyed == 1);
    server->stop(); thread.join();
    CHECK(run_ok && server->stats().validation_errors == 0);
    std::printf("production Texture Feature: create/replace/region/release, creator close, stale revision; warnings=%llu\n",
        server->stats().validation_warnings);
}
