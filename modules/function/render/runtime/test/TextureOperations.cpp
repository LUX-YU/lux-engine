#include "CompileWorker.hpp"
#include "TextureFixture.hpp"
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/render/features/Texture.hpp>

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

static TexturePublished published(RenderCompletion result)
{
    CHECK(result.header.outcome == EOperationOutcome::SUCCESS);
    return take(decodeWireBytes<TexturePublished>({*result.schema, result.bytes(), {}}, {128, 0}));
}

int main()
{
    CompileWorker host;
    const auto callbacks = host.configure({});
    RenderServerConfig config{"runtime.texture.operations", true};
    config.transfer_queues = true;
    config.texture_capacity = 16;
    config.uploads = {1024 * 1024, 2, 128 * 1024 * 1024,
        callbacks.compile_user, callbacks.enqueue_compile, callbacks.join_compile};
    auto server = take(RenderServer::create(config));
    CHECK(server->registerStorage<lux::render::TextureStorage>());
    bool run_ok = false;
    std::thread thread([&] { run_ok = bool(server->run()); });
    auto a = take(RenderClient::create(config.channel_name));
    auto b = take(RenderClient::create(config.channel_name));
    CHECK(wait(*a, take(a->hello())).header.outcome == EOperationOutcome::SUCCESS);
    CHECK(wait(*b, take(b->hello())).header.outcome == EOperationOutcome::SUCCESS);
    auto bound_a = take(a->bind<UploadTexture, TexturePublished>(uploadTextureOperation()));
    auto bound_b = take(b->bind<UploadTexture, TexturePublished>(uploadTextureOperation()));
    auto info = information(64, 4, false);
    const auto last = info.mip_ranges[3];
    std::atomic<unsigned> destroyed{0};
    auto asset = std::shared_ptr<const std::vector<std::byte>>(
        new std::vector<std::byte>(last.offset + last.size, std::byte{31}),
        [&](auto* value) { delete value; ++destroyed; });
    std::array payload{take(RenderPayload::fromVector(asset, 0, asset->size()))};
    UploadTexture input{{}, 0, info, {0, 0, asset->size()}};
    auto malformed = input;
    malformed.source.attachment_index = 1;
    CHECK(!a->submit(bound_a, malformed, payload));
    CHECK(!b->submit(bound_b, malformed, payload)); // rejected submissions also consume/recycle a generation
    malformed.info.mip_ranges[0].size = UINT64_MAX;
    malformed.source.attachment_index = 0;
    CHECK(wait(*a, take(a->submit(bound_a, malformed, payload))).header.outcome == EOperationOutcome::FAILED);
    CHECK(wait(*b, take(b->submit(bound_b, malformed, payload))).header.outcome == EOperationOutcome::FAILED);
    // Same numeric RequestId on distinct physical clients never returns the other TextureId.
    auto id_a = take(a->submit(bound_a, input, payload));
    auto id_b = take(b->submit(bound_b, input, payload));
    CHECK(id_a == id_b);
    auto texture_a = published(wait(*a, id_a));
    auto texture_b = published(wait(*b, id_b));
    CHECK(texture_a.texture != texture_b.texture && texture_a.revision == 1 && texture_b.revision == 1);
    a.reset(); // creator is not the Global storage owner
    input.texture = texture_a.texture;
    input.expected_revision = 1;
    texture_a = published(wait(*b, take(b->submit(bound_b, input, payload))));
    CHECK(texture_a.revision == 2);
    CHECK(wait(*b, take(b->submit(bound_b, input, payload))).header.outcome == EOperationOutcome::FAILED);
    auto region = take(b->bind<UploadTextureRegion, TexturePublished>(uploadTextureRegionOperation()));
    auto patch = std::make_shared<const std::vector<std::byte>>(28, std::byte{92});
    std::array patch_payload{take(RenderPayload::fromVector(patch, 0, patch->size()))};
    texture_a = published(wait(*b, take(b->submit(region,
        UploadTextureRegion{texture_a.texture, 2, {1, 2, 1, 3, 2, 16}, {0, 0, patch->size()}}, patch_payload))));
    CHECK(texture_a.revision == 3);
    auto release = take(b->bind<TexturePublished>(releaseTextureOperation()));
    CHECK(wait(*b, take(b->submit(release, texture_a))).header.outcome == EOperationOutcome::SUCCESS);
    CHECK(wait(*b, take(b->submit(release, texture_a))).header.outcome == EOperationOutcome::FAILED);
    CHECK(wait(*b, take(b->submit(release, texture_b))).header.outcome == EOperationOutcome::SUCCESS);
    input.texture = {}; input.expected_revision = 0;
    const auto replacement = published(wait(*b, take(b->submit(bound_b, input, payload))));
    CHECK(replacement.texture != texture_a.texture && replacement.texture != texture_b.texture);
    CHECK(wait(*b, take(b->submit(release, replacement))).header.outcome == EOperationOutcome::SUCCESS);
    asset.reset(); payload = {};
    CHECK(destroyed == 1 && b->retainedBytes() == 0);
    server->stop(); thread.join();
    CHECK(run_ok && server->stats().validation_errors == 0);
    std::printf("production Texture Feature: create/replace/region/release, creator close, stale revision; warnings=%llu\n",
        server->stats().validation_warnings);
}
