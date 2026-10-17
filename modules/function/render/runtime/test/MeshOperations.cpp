#include "../../native/vulkan/test/Support.hpp"
#include "Discovery.hpp"
#include "ServerFixture.hpp"
#include "TypedTest.hpp"
#include <lux/engine/render/native/vulkan/mesh/MeshCommandHandler.hpp>
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/render/resource/Mesh.hpp>
#include <cstring>

using namespace foundation_test;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;



int main()
{
    CpuFixture host;
    RenderServerConfig config{"runtime.mesh.operations", true};
    vulkan::DeviceOptions config_device{.dynamic_rendering = true};
    config_device.multiple_queues = true;
    config.uploads = TransferOptions{65536, 2, 131072};
    auto server = take(createTestServer(host, config, config_device));
    const auto type = take(server->registerStorage<lux::render::MeshStorage>());
    CHECK(take(server->registerStorage<lux::render::MeshStorage>()) == type);
    bool run_ok = false;
    const auto directory_id = take(server->commandId<ListCommands>());
    std::thread thread([&] { run_ok = bool(server->run()); });
    auto a = take(runtime_test::connect(config.channel_name, directory_id));
    auto b = take(runtime_test::connect(config.channel_name, directory_id));
    CHECK(runtime_test::refresh(*a) && runtime_test::refresh(*b));
    const auto storage = take(runtime_test::prepareStorage(*a, kMeshStorageType.name(), "mesh"));
    CHECK(take(runtime_test::prepareStorage(*b, kMeshStorageType.name(), "mesh")) == storage);
    const auto set_a = take(runtime_test::find<SetMesh>(*a)), set_b = take(runtime_test::find<SetMesh>(*b));
    const auto query = take(runtime_test::find<QueryMesh>(*b));
    const auto release = take(runtime_test::find<ReleaseMesh>(*b));
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
        auto failed = call<SetMesh>(*a, set_a, input);
        CHECK(!failed.get<SetMesh::Response>());
    }
    input.expected_revision = 0;
    auto other = input;
    // Balance one failed request on B so equal numbers still exercise separate physical origins.
    other.expected_revision = 7;
    CHECK(call<SetMesh>(*b, set_b, std::move(other)).get<SetMesh::Response>().has_value() == false);
    other = input;
    const auto id_a = take(runtime_test::send<SetMesh>(*a, set_a, input));
    const auto id_b = take(runtime_test::send<SetMesh>(*b, set_b, other));
    CHECK(id_a == id_b);
    MeshPublished mesh_a, mesh_b;
    MeshDescription description = input.description;
    {
        auto result_a = wait(*a, id_a);
        auto result_b = wait(*b, id_b);
        CHECK(result_a.get<SetMesh::Response>().has_value());
        CHECK(result_b.get<SetMesh::Response>().has_value());
        mesh_a = result_a.get<SetMesh::Response>().value(); mesh_b = result_b.get<SetMesh::Response>().value();
        CHECK(mesh_a.mesh != mesh_b.mesh);
    }
    a.reset();
    input.mesh = mesh_a.mesh;
    input.expected_revision = mesh_a.revision;
    std::optional replaced{call<SetMesh>(*b, set_b, input)};
    CHECK(replaced->get<SetMesh::Response>().has_value() && replaced->get<SetMesh::Response>()->revision == 2);
    auto information = call<QueryMesh>(*b, query, mesh_a.mesh);
    CHECK(information.get<QueryMesh::Response>().has_value());
    CHECK(information.get<QueryMesh::Response>()->description == description && information.get<QueryMesh::Response>()->bytes == 60);
    CHECK(call<ReleaseMesh>(*b, release, mesh_a).get<ReleaseMesh::Response>().has_value() == false);
    CHECK(call<ReleaseMesh>(*b, release, replaced->get<SetMesh::Response>().value()).get<ReleaseMesh::Response>().has_value());
    CHECK(call<QueryMesh>(*b, query, mesh_a.mesh).get<QueryMesh::Response>().has_value() == false);
    CHECK(call<ReleaseMesh>(*b, release, mesh_b).get<ReleaseMesh::Response>().has_value());
    server->stop(); thread.join();
    CHECK(run_ok && server->stats().validation_errors == 0);
    server.reset(); b.reset(); asset.reset();
    CHECK(destroyed == 0); // The application retained its input for retry.
    replaced.reset();
    input.source = {};
    CHECK(destroyed == 0); // The second application request still owns the same backing.
    other.source = {};
    CHECK(destroyed == 1);
    std::printf("Mesh typed Commands: two clients, explicit retained-input retry, real upload, "
        "query/revision/release and source late destruction\n");
}
