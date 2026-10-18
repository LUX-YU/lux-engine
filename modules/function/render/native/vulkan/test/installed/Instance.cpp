#include "../TextureFixture.hpp"
#include <lux/engine/render/native/vulkan/material/MaterialCommandHandler.hpp>
#include <lux/engine/render/native/vulkan/material/MaterialStorage.hpp>
#include <fstream>
#include <lux/engine/render/native/vulkan/mesh/MeshStorage.hpp>
#include <lux/engine/render/native/vulkan/instance/InstanceStorage.hpp>
#include <lux/engine/render/native/vulkan/instance/InstanceCommandHandler.hpp>

int main(int argc, char** argv)
{
    CHECK(argc == 2);
    std::ifstream input(argv[1], std::ios::binary | std::ios::ate);
    CHECK(input);
    const auto bytes = input.tellg();
    CHECK(bytes > 0 && bytes % 4 == 0);
    MaterialProgram material;
    material.fragment = {rdesc::EShaderType::FRAGMENT, "main", std::vector<std::uint32_t>(std::size_t(bytes) / 4)};
    input.seekg(0);
    CHECK(input.read(reinterpret_cast<char*>(material.fragment.words.data()), bytes));
    material.parameter_count = 1;
    material.defaults[0] = {.25f, .5f, .75f, 1};
    auto program = take(ConfigValue::make(std::move(material)));
    Validation validation;
    CpuFixture host;
    auto bootstrap = instance(validation);
    auto session = take(DeviceSession::create(bootstrap));
    CHECK(session->configureUploads({65536, 1, 65536}, host.cpu()));
    const auto factory = StorageProvider<lux::render::MaterialStorage>::factory();
    CHECK(session->resources().installFactory(factory, MaterialStorageConfig{}));
    const auto id = take(session->globalStorages().getOrCreate(session->resources().typeId(kMaterialStorageType), "material"));
    auto& storage = *take(session->resources().borrow<lux::render::vulkan::MaterialStorage>(id));
    const MaterialWrite request{{id, {}}, 0, program, program.borrow<MaterialProgram>()->defaults};
    auto upload = take(lux::render::upload(storage, request));
    complete(upload.second, host, *session);
    const auto published = upload.first;
    upload.second.reset();
    const auto access = take(session->resources().tryGet<lux::render::MaterialStorage>(id));
    CHECK(take(access.query(published.material)).parameters == request.parameters);
    CHECK(session->resources().installFactory(meshStorageFactory(), MeshStorageConfig{}));
    CHECK(session->resources().installFactory(StorageProvider<lux::render::InstanceStorage>::factory(), InstanceStorageConfig{}));
    const auto mesh_id = take(session->globalStorages().getOrCreate(session->resources().typeId(kMeshStorageType), "mesh"));
    const auto instance_id = take(session->globalStorages().getOrCreate(session->resources().typeId(kInstanceStorageType), "instances"));
    auto& meshes = *take(session->resources().borrow<lux::render::vulkan::MeshStorage>(mesh_id));
    auto& instances = *take(session->resources().borrow<lux::render::vulkan::InstanceStorage>(instance_id));
    auto owner = std::make_shared<std::vector<std::byte>>(60);
    const std::array<unsigned, 3> indices{0, 1, 2};
    std::memcpy(owner->data() + 48, indices.data(), 12);
    MeshWrite mesh_input{{mesh_id, {}}, 0,
        {16, 3, 48, 4, 3, {{0, 0, rdesc::ETextureFormat::RGBA32_SFLOAT}}, {{0, 3, 0}}}, payload(owner)};
    auto mesh_upload = take(lux::render::upload(meshes, mesh_input));
    complete(mesh_upload.second, host, *session);
    const auto mesh = mesh_upload.first;
    mesh_upload.second.reset();
    auto record = take(instances.create({mesh.mesh, published.material}));
    TransformBatch update{instance_id, 51, {{record.instance, record.revision, kIdentityTransform}}};
    update.updates[0].transform[12] = .75f;
    CHECK(take(instances.update(update)).sequence == 51);
    ++record.revision;
    CHECK(take(instances.query(record.instance)).description.transform[12] == .75f);
    CHECK(instances.release(record));
    CHECK(!instances.query(record.instance));
    CHECK(meshes.release(mesh.mesh, mesh.revision));
    CHECK(storage.release(published.material, published.revision));
    CHECK(session->drain());
    session.reset();
    CHECK(validation.errors == 0);
}
