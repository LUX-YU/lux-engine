#include "ServerFixture.hpp"
#include "TypedTest.hpp"
#include "TextureFixture.hpp"
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/render/resource/Texture.hpp>

using namespace std::chrono_literals;

static RenderCompletion wait(RenderClient& client, RequestId id)
{
    const auto end = Clock::now() + 15s;
    while (Clock::now() < end)
    {
        RenderCompletion completion;
        const auto state = take(client.tryReceive(completion));
        CHECK(state != ERenderReceive::CLOSED);
        if (state == ERenderReceive::RECEIVED) { CHECK(completion.header.request == id); return completion; }
        std::this_thread::yield();
    }
    std::abort();
}

template <class C> static TexturePublished published(RenderCompletion result, const BoundCommand<C>& binding)
{
    CHECK(result.header.outcome == ECommandOutcome::SUCCESS);
    return take(std::move(result).take(binding)).response();
}

int main()
{
    CpuFixture host;
    RenderServerConfig config{"runtime.texture.operations", true};
    config.transfer_queues = true;
    config.uploads = TransferOptions{1024 * 1024, 2, 128 * 1024 * 1024};
    auto server = take(RenderServer::create(withCpu(host, config)));
    CHECK(server->registerStorage<lux::render::TextureStorage>({16, 256 * 1024 * 1024}));
    bool run_ok = false;
    std::thread thread([&] { run_ok = bool(server->run()); });
    auto a = take(RenderClient::create(config.channel_name));
    auto b = take(RenderClient::create(config.channel_name));
    CHECK(wait(*a, take(a->hello())).header.outcome == ECommandOutcome::SUCCESS);
    CHECK(wait(*b, take(b->hello())).header.outcome == ECommandOutcome::SUCCESS);
    auto bound_a = take(a->bind<SetTexture>());
    auto bound_b = take(b->bind<SetTexture>());
    auto info = information(64, 4, false);
    const auto last = info.mip_ranges[3];
    std::atomic<unsigned> destroyed{0};
    auto asset = std::shared_ptr<const std::vector<std::byte>>(
        new std::vector<std::byte>(last.offset + last.size, std::byte{31}),
        [&](auto* value) { delete value; ++destroyed; });
    std::array payload{take(RenderPayload::fromVector(asset, 0, asset->size()))};
    SetTexture::Request input{{}, 0, info, payload[0]};
    // Typed messages cannot express an out-of-range external attachment index. An empty source
    // remains a real native upload boundary error, and both clients recycle the failed request.
    auto malformed = input;
    malformed.source = {};
    CHECK(wait(*a, take(submitValue(*a, bound_a, malformed))).header.outcome == ECommandOutcome::FAILED);
    CHECK(wait(*b, take(submitValue(*b, bound_b, malformed))).header.outcome == ECommandOutcome::FAILED);
    malformed = input;
    malformed.info.mip_ranges[0].size = UINT64_MAX;
    CHECK(wait(*a, take(submitValue(*a, bound_a, malformed))).header.outcome == ECommandOutcome::FAILED);
    CHECK(wait(*b, take(submitValue(*b, bound_b, malformed))).header.outcome == ECommandOutcome::FAILED);
    // Same numeric RequestId on distinct physical clients never returns the other TextureId.
    auto id_a = take(submitValue(*a, bound_a, input));
    auto id_b = take(submitValue(*b, bound_b, input));
    CHECK(id_a == id_b);
    auto texture_a = published(wait(*a, id_a), bound_a);
    auto texture_b = published(wait(*b, id_b), bound_b);
    CHECK(texture_a.texture != texture_b.texture && texture_a.revision == 1 && texture_b.revision == 1);
    a.reset(); // creator is not the Global storage owner
    input.texture = texture_a.texture;
    input.expected_revision = 1;
    texture_a = published(wait(*b, take(submitValue(*b, bound_b, input))), bound_b);
    CHECK(texture_a.revision == 2);
    CHECK(wait(*b, take(submitValue(*b, bound_b, input))).header.outcome == ECommandOutcome::FAILED);
    auto region = take(b->bind<SetTextureRegion>());
    auto patch = std::make_shared<const std::vector<std::byte>>(28, std::byte{92});
    std::array patch_payload{take(RenderPayload::fromVector(patch, 0, patch->size()))};
    texture_a = published(wait(*b, take(submitValue(*b, region,
        SetTextureRegion::Request{texture_a.texture, 2, {1, 2, 1, 3, 2, 16}, patch_payload[0]}))), region);
    CHECK(texture_a.revision == 3);
    auto query = take(b->bind<QueryTexture>());
    const auto current = published(wait(*b, take(submitValue(*b, query, texture_a.texture))), query);
    CHECK(current.texture == texture_a.texture && current.revision == texture_a.revision);
    CHECK(wait(*b, take(submitValue(*b, query, TextureId{}))).header.outcome == ECommandOutcome::FAILED);
    auto release = take(b->bind<ReleaseTexture>());
    CHECK(wait(*b, take(submitValue(*b, release, texture_a))).header.outcome == ECommandOutcome::SUCCESS);
    CHECK(wait(*b, take(submitValue(*b, query, texture_a.texture))).header.outcome == ECommandOutcome::FAILED);
    CHECK(wait(*b, take(submitValue(*b, release, texture_a))).header.outcome == ECommandOutcome::FAILED);
    CHECK(wait(*b, take(submitValue(*b, release, texture_b))).header.outcome == ECommandOutcome::SUCCESS);
    input.texture = {}; input.expected_revision = 0;
    const auto replacement = published(wait(*b, take(submitValue(*b, bound_b, input))), bound_b);
    CHECK(replacement.texture != texture_a.texture && replacement.texture != texture_b.texture);
    CHECK(wait(*b, take(submitValue(*b, release, replacement))).header.outcome == ECommandOutcome::SUCCESS);
    asset.reset(); payload = {}; input.source = {}; malformed.source = {};
    CHECK(destroyed == 1 && b->retainedBytes() == 0);
    server->stop(); thread.join();
    CHECK(run_ok && server->stats().validation_errors == 0);
    std::printf("production Texture Feature: create/replace/region/release, creator close, stale revision; warnings=%llu\n",
        server->stats().validation_warnings);
}
