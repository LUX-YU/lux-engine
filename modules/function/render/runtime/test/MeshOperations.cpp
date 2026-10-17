#include "../../native/vulkan/test/Support.hpp"
#include "Discovery.hpp"
#include "ServerFixture.hpp"
#include "TypedTest.hpp"
#include <lux/engine/render/native/vulkan/mesh/MeshCommands.hpp>
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/render/resource/Mesh.hpp>
#include <cstring>

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
    auto result = wait(client, take(client.submit(binding, input, 4096)));
    return take(std::move(result).take(binding));
}

int main()
{
    CpuFixture host;
    RenderServerConfig config{"runtime.mesh.operations", true};
    config.device.multiple_queues = true;
    config.uploads = TransferOptions{65536, 2, 131072};
    auto server = take(RenderServer::create(withCpu(host, config)));
    const auto type = take(server->registerStorage<lux::render::MeshStorage>());
    CHECK(take(server->registerStorage<lux::render::MeshStorage>()) == type);
    bool run_ok = false;
    std::thread thread([&] { run_ok = bool(server->run()); });
    auto a = take(RenderClient::create(config.channel_name));
    auto b = take(RenderClient::create(config.channel_name));
    CHECK(wait(*a, take(a->hello())).header.outcome == ECommandOutcome::SUCCESS);
    CHECK(wait(*b, take(b->hello())).header.outcome == ECommandOutcome::SUCCESS);
    CHECK(runtime_test::refresh(*a) && runtime_test::refresh(*b));
    const auto storage = take(runtime_test::prepareStorage(*a, kMeshStorageType.name(), "mesh"));
    CHECK(take(runtime_test::prepareStorage(*b, kMeshStorageType.name(), "mesh")) == storage);
    const auto set_a = take(a->bind<SetMesh>()), set_b = take(b->bind<SetMesh>());
    const auto query = take(b->bind<QueryMesh>());
    const auto release = take(b->bind<ReleaseMesh>());
    std::atomic<unsigned> destroyed{0};
    auto mutable_asset = new std::vector<std::byte>(60);
    const std::array<float, 12> vertices{-1, -1, 0, .25f, 3, -1, 0, .25f, -1, 3, 0, .25f};
    const std::array<unsigned, 3> indices{0, 1, 2};
    std::memcpy(mutable_asset->data(), vertices.data(), sizeof(vertices));
    std::memcpy(mutable_asset->data() + 48, indices.data(), sizeof(indices));
    auto asset = std::shared_ptr<const std::vector<std::byte>>(mutable_asset,
        [&](auto* value) { delete value; ++destroyed; });
    MeshWrite input{{storage, {}}, 0, {16, 3, 48, 4, 3,
        {{0, 0, lux::rdesc::ETextureFormat::RGBA32_SFLOAT}}, {{0, 3, 0}}},
        take(RenderPayload::fromVector(asset, 0, asset->size()))};
    input.description.attributes.reserve(31);
    const auto* original_attributes = input.description.attributes.data();
    const auto* original_bytes = input.source.bytes().view().data();
    input.expected_revision = 7;
    {
        auto failed = call(*a, set_a, std::move(input));
        CHECK(failed.header().outcome == ECommandOutcome::FAILED);
        CHECK(failed.request().description.attributes.data() == original_attributes);
        CHECK(failed.request().description.attributes.capacity() == 31);
        CHECK(failed.request().source.bytes().view().data() == original_bytes);
        input = std::move(failed.request());
    }
    CHECK(a->retainedBytes() == 0);
    input.expected_revision = 0;
    auto other = input;
    // Balance one failed request on B so equal numbers still exercise separate physical origins.
    other.expected_revision = 7;
    CHECK(call(*b, set_b, std::move(other)).header().outcome == ECommandOutcome::FAILED);
    other = input;
    const auto id_a = take(a->submit(set_a, input, 4096));
    const auto id_b = take(b->submit(set_b, other, 4096));
    CHECK(id_a == id_b);
    MeshPublished mesh_a, mesh_b;
    MeshDescription description;
    {
        auto result_a = take(wait(*a, id_a).take(set_a));
        auto result_b = take(wait(*b, id_b).take(set_b));
        CHECK(result_a.header().outcome == ECommandOutcome::SUCCESS);
        CHECK(result_b.header().outcome == ECommandOutcome::SUCCESS);
        mesh_a = result_a.response(); mesh_b = result_b.response();
        CHECK(mesh_a.mesh != mesh_b.mesh);
        description = result_a.request().description;
        input = std::move(result_a.request());
    }
    CHECK(a->retainedBytes() == 0 && b->retainedBytes() == 0);
    a.reset();
    input.mesh = mesh_a.mesh;
    input.expected_revision = mesh_a.revision;
    std::optional replaced{call(*b, set_b, std::move(input))};
    CHECK(replaced->header().outcome == ECommandOutcome::SUCCESS && replaced->response().revision == 2);
    auto information = call(*b, query, mesh_a.mesh);
    CHECK(information.header().outcome == ECommandOutcome::SUCCESS);
    CHECK(information.response().description == description && information.response().bytes == 60);
    CHECK(call(*b, release, mesh_a).header().outcome == ECommandOutcome::FAILED);
    CHECK(call(*b, release, replaced->response()).header().outcome == ECommandOutcome::SUCCESS);
    CHECK(call(*b, query, mesh_a.mesh).header().outcome == ECommandOutcome::FAILED);
    CHECK(call(*b, release, mesh_b).header().outcome == ECommandOutcome::SUCCESS);
    server->stop(); thread.join();
    CHECK(run_ok && server->stats().validation_errors == 0);
    server.reset(); b.reset(); asset.reset();
    CHECK(destroyed == 0); // Returned Request remains the actual source Owner after Server/Client destruction.
    CHECK(replaced->request().source.bytes().view().data() == original_bytes);
    replaced.reset();
    CHECK(destroyed == 1);
    std::printf("Mesh typed Commands: two clients, failed vector recovery, full budget refund, real upload, "
        "query/revision/release and source late destruction\n");
}
