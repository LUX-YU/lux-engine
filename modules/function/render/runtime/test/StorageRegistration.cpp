#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/render/features/Texture.hpp>

#include <cstdio>

using namespace lux::render;

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #value); return 1; } } while (false)

int main()
{
    auto created = RenderServer::create({"test.storage.registration"});
    CHECK(created);
    auto& server = **created;
    auto registration = TextureStorage::registration();
    auto missing = registration;
    missing.factory.create = nullptr;
    CHECK(!server.registerStorage(missing));
    auto other_api = registration;
    other_api.factory.api = EGraphicsApi::CONTRACT_TEST;
    CHECK(!server.registerStorage(other_api));
    auto mismatch = registration;
    auto wrong_description = TextureStorage::description;
    wrong_description.abi.minor = 1;
    mismatch.factory.description = &wrong_description;
    CHECK(!server.registerStorage(mismatch));
    auto bad_handler = registration;
    auto handler = registration.handlers[0];
    handler.gpu_completion = false;
    bad_handler.handlers = {&handler, 1};
    CHECK(!server.registerStorage(bad_handler));
    CHECK(server.registerStorage<TextureStorage>());
    CHECK(!server.registerStorage<TextureStorage>());
    // No device, staging page, image or descriptor is created by the registration above.
    // Real upload/region/release and two-client Manifest assertions live in texture_operations.
    CHECK(!server.registerStorage(mismatch));
    return 0;
}
