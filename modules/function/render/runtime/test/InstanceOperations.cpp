#include "../../native/vulkan/test/Support.hpp"
#include "Discovery.hpp"
#include "ServerFixture.hpp"
#include "TypedTest.hpp"
#include <lux/engine/render/native/vulkan/material/MaterialCommands.hpp>
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/render/resource/Material.hpp>
#include <fstream>
#include <lux/engine/render/native/vulkan/mesh/MeshCommands.hpp>
#include <lux/engine/render/native/vulkan/instance/InstanceCommands.hpp>

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


int main(int argc, char** argv)
{
    CHECK(argc == 2);
    std::ifstream source(argv[1], std::ios::binary | std::ios::ate);
    CHECK(source);
    const auto bytes = source.tellg();
    CHECK(bytes > 0 && bytes % 4 == 0);
    MaterialProgram program;
    program.fragment = {lux::rdesc::EShaderType::FRAGMENT, "main", std::vector<std::uint32_t>(std::size_t(bytes) / 4)};
    source.seekg(0);
    CHECK(source.read(reinterpret_cast<char*>(program.fragment.words.data()), bytes));
    program.parameter_count = 1;
    program.defaults[0] = {1, 1, 1, 1};
    auto cooked = take(ConfigValue::make(std::move(program)));
    CpuFixture host;
    RenderServerConfig config{"runtime.instance.operations", true};
    config.uploads = TransferOptions{65536, 2, 131072};
    auto server = take(RenderServer::create(withCpu(host, config)));
    CHECK(server->registerStorage<lux::render::MaterialStorage>());
    CHECK(server->registerStorage<lux::render::MeshStorage>());
    CHECK(server->registerStorage<lux::render::InstanceStorage>());
    bool run_ok = false;
    std::thread thread([&] { run_ok = bool(server->run()); });
    auto client = take(RenderClient::create(config.channel_name));
    CHECK(wait(*client, take(client->hello())).header.outcome == ECommandOutcome::SUCCESS);
    CHECK(runtime_test::refresh(*client));
    const auto material_storage = take(runtime_test::prepareStorage(*client, kMaterialStorageType.name(), "material"));
    const auto mesh_storage = take(runtime_test::prepareStorage(*client, kMeshStorageType.name(), "mesh"));
    const auto instance_storage = take(runtime_test::prepareStorage(*client, kInstanceStorageType.name(), "instance"));
    const auto material_set = take(client->bind<SetMaterial>());
    const auto mesh_set = take(client->bind<SetMesh>());
    const auto create = take(client->bind<CreateInstance>());
    const auto update = take(client->bind<UpdateTransforms>());
    const auto query = take(client->bind<QueryInstance>());
    const auto release = take(client->bind<ReleaseInstance>());
    MaterialId material;
    {
        auto result = call(*client, material_set, {{material_storage, {}}, 0, cooked,
            cooked.borrow<MaterialProgram>()->defaults});
        CHECK(result.header().outcome == ECommandOutcome::SUCCESS);
        material = result.response().material;
    }
    auto owner = std::make_shared<std::vector<std::byte>>(60);
    const std::array<unsigned, 3> indices{0, 1, 2};
    std::memcpy(owner->data() + 48, indices.data(), 12);
    MeshId mesh;
    {
        auto result = call(*client, mesh_set, {{mesh_storage, {}}, 0,
            {16, 3, 48, 4, 3, {{0, 0, lux::rdesc::ETextureFormat::RGBA32_SFLOAT}}, {{0, 3, 0}}},
            take(RenderPayload::fromVector(owner, 0, owner->size()))});
        CHECK(result.header().outcome == ECommandOutcome::SUCCESS);
        mesh = result.response().mesh;
    }
    std::vector<InstancePublished> instances;
    for (unsigned i = 0; i < 100; ++i)
    {
        auto result = call(*client, create, {instance_storage, {mesh, material}});
        CHECK(result.header().outcome == ECommandOutcome::SUCCESS);
        instances.push_back(result.response());
    }
    TransformBatch input{instance_storage, 53};
    input.updates.reserve(257);
    for (auto id : instances) input.updates.push_back({id.instance, id.revision, kIdentityTransform});
    const auto* backing = input.updates.data();
    const auto capacity = input.updates.capacity();
    input.updates.back().expected_revision = 0;
    {
        auto failed = call(*client, update, std::move(input));
        CHECK(failed.header().outcome == ECommandOutcome::FAILED);
        CHECK(failed.request().updates.data() == backing && failed.request().updates.capacity() == capacity);
        input = std::move(failed.request());
    }
    CHECK(client->retainedBytes() == 0);
    CHECK(call(*client, query, instances.front().instance).response().published.revision == 1);
    input.updates.back().expected_revision = 1;
    input.updates.push_back(input.updates.front());
    input.updates.back().transform[12] = .5f;
    {
        auto result = call(*client, update, std::move(input));
        CHECK(result.header().outcome == ECommandOutcome::SUCCESS);
        CHECK(result.response().sequence == 53 && result.response().changed == 100);
        CHECK(result.request().updates.data() == backing && result.request().updates.capacity() == capacity);
        input = std::move(result.request());
    }
    CHECK(client->retainedBytes() == 0);
    CHECK(call(*client, query, instances.front().instance).response().description.transform[12] == .5f);
    input.sequence = 54;
    // A stale revision n does not overwrite or acknowledge n+1.
    CHECK(call(*client, update, std::move(input)).header().outcome == ECommandOutcome::FAILED);
    CHECK(call(*client, query, instances.front().instance).response().published.revision == 2);
    for (auto id : instances)
    {
        id.revision = 2;
        CHECK(call(*client, release, id).header().outcome == ECommandOutcome::SUCCESS);
        CHECK(call(*client, query, id.instance).header().outcome == ECommandOutcome::FAILED);
    }
    server->stop(); thread.join();
    CHECK(run_ok && server->stats().validation_errors == 0 && client->retainedBytes() == 0);
    std::puts("Instance typed batch: all-or-none, Last-Wins, original capacity, budget, exact revision PASS");
}
