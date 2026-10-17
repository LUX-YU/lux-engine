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
    const auto directory_id = take(server->commandId<ListCommands>());
    std::thread thread([&] { run_ok = bool(server->run()); });
    auto client = take(runtime_test::connect(config.channel_name, directory_id));
    CHECK(runtime_test::refresh(*client));
    const auto material_storage = take(runtime_test::prepareStorage(*client, kMaterialStorageType.name(), "material"));
    const auto mesh_storage = take(runtime_test::prepareStorage(*client, kMeshStorageType.name(), "mesh"));
    const auto instance_storage = take(runtime_test::prepareStorage(*client, kInstanceStorageType.name(), "instance"));
    const auto material_set = take(runtime_test::find<SetMaterial>(*client));
    const auto mesh_set = take(runtime_test::find<SetMesh>(*client));
    const auto create = take(runtime_test::find<CreateInstance>(*client));
    const auto update = take(runtime_test::find<UpdateTransforms>(*client));
    const auto query = take(runtime_test::find<QueryInstance>(*client));
    const auto release = take(runtime_test::find<ReleaseInstance>(*client));
    MaterialId material;
    {
        auto result = call<SetMaterial>(*client, material_set, {{material_storage, {}}, 0, cooked,
            cooked.borrow<MaterialProgram>()->defaults});
        CHECK(result.metadata<ReplyHeader>().outcome == ECommandOutcome::SUCCESS);
        material = result.get<SetMaterial::Response>().material;
    }
    auto owner = std::make_shared<std::vector<std::byte>>(60);
    const std::array<unsigned, 3> indices{0, 1, 2};
    std::memcpy(owner->data() + 48, indices.data(), 12);
    MeshId mesh;
    {
        auto result = call<SetMesh>(*client, mesh_set, {{mesh_storage, {}}, 0,
            {16, 3, 48, 4, 3, {{0, 0, lux::rdesc::ETextureFormat::RGBA32_SFLOAT}}, {{0, 3, 0}}},
            take(RenderPayload::fromVector(owner, 0, owner->size()))});
        CHECK(result.metadata<ReplyHeader>().outcome == ECommandOutcome::SUCCESS);
        mesh = result.get<SetMesh::Response>().mesh;
    }
    std::vector<InstancePublished> instances;
    for (unsigned i = 0; i < 100; ++i)
    {
        auto result = call<CreateInstance>(*client, create, {instance_storage, {mesh, material}});
        CHECK(result.metadata<ReplyHeader>().outcome == ECommandOutcome::SUCCESS);
        instances.push_back(result.get<CreateInstance::Response>());
    }
    TransformBatch input{instance_storage, 53};
    input.updates.reserve(257);
    for (auto id : instances) input.updates.push_back({id.instance, id.revision, kIdentityTransform});
    const auto* backing = input.updates.data();
    const auto capacity = input.updates.capacity();
    input.updates.back().expected_revision = 0;
    {
        auto failed = call<UpdateTransforms>(*client, update, input);
        CHECK(failed.metadata<ReplyHeader>().outcome == ECommandOutcome::FAILED);
    }
    CHECK(call<QueryInstance>(*client, query, instances.front().instance).get<QueryInstance::Response>().published.revision == 1);
    input.updates.back().expected_revision = 1;
    input.updates.push_back(input.updates.front());
    input.updates.back().transform[12] = .5f;
    {
        auto result = call<UpdateTransforms>(*client, update, input);
        CHECK(result.metadata<ReplyHeader>().outcome == ECommandOutcome::SUCCESS);
        CHECK(result.get<UpdateTransforms::Response>().sequence == 53 && result.get<UpdateTransforms::Response>().changed == 100);
    }
    CHECK(call<QueryInstance>(*client, query, instances.front().instance).get<QueryInstance::Response>().description.transform[12] == .5f);
    input.sequence = 54;
    // A stale revision n does not overwrite or acknowledge n+1.
    CHECK(call<UpdateTransforms>(*client, update, input).metadata<ReplyHeader>().outcome == ECommandOutcome::FAILED);
    CHECK(call<QueryInstance>(*client, query, instances.front().instance).get<QueryInstance::Response>().published.revision == 2);
    for (auto id : instances)
    {
        id.revision = 2;
        CHECK(call<ReleaseInstance>(*client, release, id).metadata<ReplyHeader>().outcome == ECommandOutcome::SUCCESS);
        CHECK(call<QueryInstance>(*client, query, id.instance).metadata<ReplyHeader>().outcome == ECommandOutcome::FAILED);
    }
    server->stop(); thread.join();
    CHECK(run_ok && server->stats().validation_errors == 0);
    std::puts("Instance batch: all-or-none, Last-Wins, caller-owned retry input, exact revision");
}
