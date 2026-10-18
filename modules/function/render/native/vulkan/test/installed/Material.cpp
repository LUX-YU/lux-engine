#include "../TextureFixture.hpp"
#include <lux/engine/render/native/vulkan/material/MaterialCommandHandler.hpp>
#include <lux/engine/render/native/vulkan/material/MaterialStorage.hpp>
#include <fstream>

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
    CHECK(storage.release(published.material, published.revision));
    CHECK(session->drain());
    session.reset();
    CHECK(validation.errors == 0);
}
