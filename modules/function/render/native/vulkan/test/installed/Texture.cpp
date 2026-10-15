#include "../TextureFixture.hpp"
#include "../TextureSceneFixture.hpp"
#include <lux/engine/render/native/vulkan/texture/TextureStorage.hpp>
#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>

int main()
{
    Validation validation;
    PrepareWorker host;
    auto bootstrap = instance(validation);
    DeviceOptions options;
    options.dynamic_rendering = true;
    options.texture_descriptors = true;
    auto session = take(DeviceSession::create(bootstrap, options));
    CHECK(session->configureUploads({1024 * 1024, 1, 1024 * 1024,
        &host, PrepareWorker::enqueue, PrepareWorker::join}));
    CHECK(session->configureTextures(16, 1024 * 1024));
    std::vector<std::byte> input(1024, std::byte{73});
    auto upload = take(session->uploadTexture(information(16, 1, false), input));
    const auto texture = take(session->textures()->reserve(*upload));
    const auto end = Clock::now() + std::chrono::seconds(10);
    while (!take(upload->advance())) { CHECK(Clock::now() < end); std::this_thread::yield(); }
    CHECK(upload->publish()); upload.reset();
    texture_fixture::Config config{texture.texture};
    auto assembly = take(texture_fixture::assemble(config));
    auto code = std::make_shared<CompiledGraph>(take(compileGraph(*session, assembly,
        texture_fixture::programs(config), texture_fixture::target(config))));
    auto* context = take(session->prepare(code, texture_fixture::target(config), {}));
    CHECK(take(context->execute()));
    CHECK(take(session->queue().wait(*context->lastUse(), UINT64_MAX)));
    CHECK(take(context->readback()) == input);
    CHECK(session->release(context));
    CHECK(session->textures()->release(texture.texture, 1));
    code.reset(); session.reset();
    CHECK(validation.errors == 0);
}
