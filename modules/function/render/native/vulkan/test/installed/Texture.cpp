#include "../TextureFixture.hpp"
#include "../TextureSceneFixture.hpp"
#include <lux/engine/render/native/vulkan/texture/TextureStorage.hpp>
#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>

int main()
{
    Validation validation;
    CpuFixture host;
    auto bootstrap = instance(validation);
    DeviceOptions options;
    options.dynamic_rendering = true;
    options.texture_descriptors = true;
    auto session = take(DeviceSession::create(bootstrap, options));
    CHECK(session->configureUploads({1024 * 1024, 1, 1024 * 1024}, host.cpu()));
    CHECK(configureTextures(*session, 16, 1024 * 1024));
    auto input = std::make_shared<const std::vector<std::byte>>(1024, std::byte{73});
    const SetTexture::Request request{{textures(*session)->id(), {}}, 0, information(16, 1, false), payload(input)};
    auto upload = take(lux::render::upload(*textures(*session), request));
    const auto texture = upload.first;
    complete(upload.second, host, *session); upload.second.reset();
    texture_fixture::Config config{texture.texture};
    auto assembly = take(texture_fixture::assemble(config));
    auto code = std::make_shared<CompiledGraph>(take(compileGraph(*session, (assembly).definition, (assembly).analysis, (assembly).external_bindings,
        texture_fixture::programs(config), texture_fixture::target(config))));
    auto* context = take(session->prepare(code, texture_fixture::target(config), nullptr, nullptr, texture_fixture::arguments(config)));
    CHECK(take(context->execute()));
    CHECK(take(session->queue().wait(*context->lastUse(), UINT64_MAX)));
    CHECK(take(context->readback()) == *input);
    CHECK(session->release(context));
    CHECK(textures(*session)->release(texture.texture, 1));
    code.reset(); session.reset();
    CHECK(validation.errors == 0);
}
