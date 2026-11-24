#include <lux/engine/render/resource/mesh/Resource.hpp>
#include <lux/engine/render/resource/texture/Resource.hpp>
#include <lux/engine/render/resource/material/Resource.hpp>
#include <lux/engine/render/runtime/commands/MaterialCommands.hpp>
#include <lux/engine/render/runtime/commands/MeshCommands.hpp>
#include <lux/engine/description/Mesh.hpp>
#include "MaterialFixture.hpp"
#include "SceneCapture.hpp"
#include <lux/engine/core/execution/ExecutionRuntime.hpp>

int main(int argc, char** argv)
{
    using namespace lux::material;
    using R = MaterialCommands::RegisterProgram;
    using C = MaterialCommands::CreateInstance;
    using U = MaterialCommands::UnregisterProgram;
    static_assert(ipc::IpcMessage::stores_inline<C::Request>);

    DeviceOptions device;
    device.texture_descriptors = true;
    if (argc == 2 && std::string_view(argv[1]) == "d3d12") device.backend = EGraphicsBackend::D3D12;
    const auto shader_target = device.backend == EGraphicsBackend::D3D12
        ? rdesc::EShaderTarget::DXIL : rdesc::EShaderTarget::SPIRV;

    MaterialGraph graph;
    graph.shading_model = rdesc::ELightingTechnique::UNLIT;
    graph.param_slots.push_back({"albedo", EValueType::VEC4, {.25F, .5F, .75F, 1}});
    const auto surface = material_test::add(graph, MaterialOutputSurface{});
    const auto albedo = material_test::add(graph, MaterialParameter{0, EValueType::VEC4});
    CHECK(material_test::connect(graph, albedo, 0, surface, 0));
    constexpr std::array purposes{rdesc::EMaterialPurpose::FORWARD};
    const auto cooked = deriveFixtureMaterial(graph, purposes, shader_target);
    CHECK(cooked);
    const auto& program = cooked->material.programs;
    const auto& values = program.parameter_defaults;
    CHECK(values.size() == 1 && program.texture_count == 0);

    const auto runtime = lux::execution::ExecutionRuntime::create({2, 128, {128}});
    CHECK(runtime);
    RenderServerConfig config{"test.installed.material_commands", nativeValidationEnabled()};
    config.uploads.emplace();
    auto server = take(RenderServer::create(config, **runtime));
    auto client = ipc::IpcCore::instance().createRequester(config.channel_name);
    CHECK(client);
    std::thread thread([&] { CHECK(server->run()); });
    const auto commands = queryCommands(*client);
    const auto session = call<CreateDeviceSession>(*client, commands, device).device;
    const std::array current{
        CommandName{R::name}, CommandName{C::name}, CommandName{MaterialCommands::Update::name},
        CommandName{MaterialCommands::ReleaseInstance::name}, CommandName{U::name},
        CommandName{MaterialCommands::PrepareMeshPipelines::name}
    };
    const auto directory = call<QueryCommandIds>(*client, commands, current);
    CHECK(directory.ids.size() == current.size());
    for (std::size_t i = 0; i < current.size(); ++i)
    {
        const auto entry = std::ranges::find(commands.commands, current[i].name(), &CommandInfo::name);
        CHECK(entry != commands.commands.end() && entry->id == directory.ids[i]);
        std::printf("MATERIAL_COMMAND %.*s index=%u generation=%u\n",
            int(current[i].name().size()), current[i].name().data(), entry->id.index, entry->id.gen);
    }
    for (std::string_view name : {"lux.render.material.create", "lux.render.material.release"})
    {
        CHECK(std::ranges::find(commands.commands, name, &CommandInfo::name) == commands.commands.end());
        const std::array removed{CommandName{name}};
        const auto retired = call<QueryCommandIds>(*client, commands, removed, false);
        CHECK(retired.error.type == kRuntimeUnsupported && retired.ids.empty());
        std::printf("MATERIAL_COMMAND_REMOVED %.*s\n", int(name.size()), name.data());
    }
    const auto materials = call<GetOrCreateResource>(*client, commands, {session, DeviceScope{}, kMaterialResourceName});
    const auto textures = call<GetOrCreateResource>(*client, commands, {session, DeviceScope{}, kTextureResourceName});
    auto wrong_target = program;
    wrong_target.target = shader_target == rdesc::EShaderTarget::SPIRV
        ? rdesc::EShaderTarget::DXIL : rdesc::EShaderTarget::SPIRV;
    CHECK(call<R>(*client, commands, {materials.resource_id, std::move(wrong_target)}, false).error.type ==
        kDeviceUnsupported);

    const auto first = call<R>(*client, commands, {materials.resource_id, program}).program;
    const auto second = call<R>(*client, commands, {materials.resource_id, program}).program;
    CHECK(!first.isNull() && !second.isNull() && first != second);
    const auto geometry = call<GetOrCreateResource>(*client, commands, {session, DeviceScope{}, kMeshResourceName}).resource_id;
    rdesc::Mesh triangle;
    triangle.vertices.resize(3);
    triangle.vertices[0].position = {0, 0, 0};
    triangle.vertices[1].position = {1, 0, 0};
    triangle.vertices[2].position = {0, 1, 0};
    for (auto& vertex : triangle.vertices)
    {
        vertex.normal = {0, 0, 1};
        vertex.uv = {0, 0};
        vertex.tangent = {1, 0, 0};
        vertex.bitangent = {0, 1, 0};
    }
    triangle.indices = {0, 1, 2};
    triangle.bounds = lux::math::AABB{{0, 0, 0}, {1, 1, 0}};
    const auto mesh = call<MeshCommands::UploadMesh>(*client, commands, {geometry, &triangle}).mesh;
    using Prepare = MaterialCommands::PrepareMeshPipelines;
    const Prepare::Request use{first, rdesc::EMaterialPurpose::FORWARD, {{mesh}}};
    call<Prepare>(*client, commands, use);
    call<MeshCommands::ReleaseMesh>(*client, commands, {mesh});
    CHECK(call<Prepare>(*client, commands, use, false).error.type == kStorageIdentity);
    call<Prepare>(*client, commands, {first, rdesc::EMaterialPurpose::FORWARD, {}});
    std::puts("INSTALLED MESH PIPELINE PASS: prepared both windings before any Job and released the declared use");
    CHECK(call<C>(*client, commands, {first, {}, {}}, false).error.type == kStorageContract);
    const auto instance = createMaterialInstance(*client, commands, first, values);
    const auto second_instance = createMaterialInstance(*client, commands, second, values);
    CHECK(instance != second_instance);
    CHECK(call<U>(*client, commands, {first}, false).error.type == kDeviceBusy);

    using extension_detail::ResourceIdentity;
    const auto foreign = ResourceIdentity::make<MaterialProgramId>(textures.resource_id, ResourceIdentity::local(first));
    CHECK(call<C>(*client, commands, {foreign, values, {}}, false).error.type == kCommandContract);
    CHECK(call<U>(*client, commands, {foreign}, false).error.type == kCommandContract);
    call<MaterialCommands::ReleaseInstance>(*client, commands, {second_instance});
    call<U>(*client, commands, {second});
    const auto reused = call<R>(*client, commands, {materials.resource_id, program}).program;
    const auto old_key = ResourceIdentity::local(second);
    const auto new_key = ResourceIdentity::local(reused);
    CHECK(old_key.index == new_key.index && old_key.gen != new_key.gen);
    CHECK(call<C>(*client, commands, {second, values, {}}, false).error.type == kStorageIdentity);
    CHECK(call<U>(*client, commands, {second}, false).error.type == kStorageIdentity);
    CHECK(call<MaterialCommands::ReleaseInstance>(*client, commands, {second_instance}, false).error.type ==
        kStorageIdentity);

    // Remove the last resident page while two validated programs still refer to its ABI.
    call<MaterialCommands::ReleaseInstance>(*client, commands, {instance});
    call<U>(*client, commands, {first});
    const auto recreated = createMaterialInstance(*client, commands, reused, values);
    CHECK(call<U>(*client, commands, {reused}, false).error.type == kDeviceBusy);
    call<MaterialCommands::ReleaseInstance>(*client, commands, {recreated});
    call<U>(*client, commands, {reused});
    server->stop();
    thread.join();
    CHECK(server->stats().validation_errors == 0);
    std::puts("MATERIAL_COMMANDS distinct=1 target=1 parent=1 stale_generation=1 busy=1 release_order=1");
}
