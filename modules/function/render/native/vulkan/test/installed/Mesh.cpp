#include "../TextureFixture.hpp"
#include <lux/engine/render/native/vulkan/mesh/MeshCommands.hpp>
#include <lux/engine/render/native/vulkan/mesh/MeshStorage.hpp>

int main()
{
    Validation validation;
    CpuFixture host;
    auto bootstrap = instance(validation);
    auto session = take(DeviceSession::create(bootstrap));
    CHECK(session->configureUploads({65536, 1, 65536}, host.cpu()));
    const auto factory = StorageProvider<lux::render::MeshStorage>::factory();
    CHECK(session->resources().installFactory(factory, MeshStorageConfig{}));
    const auto id = take(session->globalStorages().prepare(session->resources().typeId(kMeshStorageType), "mesh"));
    auto& storage = *take(session->resources().borrow<lux::render::vulkan::MeshStorage>(id));
    auto bytes = std::make_shared<std::vector<std::byte>>(60);
    const std::array<float, 12> vertices{-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
    const std::array<unsigned, 3> indices{0, 1, 2};
    std::memcpy(bytes->data(), vertices.data(), sizeof(vertices));
    std::memcpy(bytes->data() + 48, indices.data(), sizeof(indices));
    const MeshWrite input{{id, {}}, 0, {16, 3, 48, 4, 3,
        {{0, 0, rdesc::ETextureFormat::RGBA32_SFLOAT}}, {{0, 3, 0}}}, payload(bytes)};
    auto upload = take(lux::render::upload(storage, input));
    complete(upload.second, host, *session);
    const auto published = upload.first;
    upload.second.reset();
    const auto access = take(session->resources().tryGet<lux::render::MeshStorage>(id));
    CHECK(take(access.query(published.mesh)).description == input.description);
    CHECK(storage.release(published.mesh, published.revision));
    CHECK(session->drain());
    session.reset();
    CHECK(validation.errors == 0);
}
