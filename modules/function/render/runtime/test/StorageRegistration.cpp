#include <lux/engine/render/resource/Registration.hpp>
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/render/resource/Texture.hpp>

#include <cstdio>

using namespace lux::render;

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #value); return 1; } } while (false)

int main()
{
    static_assert(std::is_copy_constructible_v<StorageRegistration>);
    static_assert(!std::is_copy_assignable_v<StorageRegistration>);
    static_assert(!std::is_move_assignable_v<StorageRegistration>);
    {
        auto limited = RenderServer::create({"test.storage.capabilities", true});
        CHECK(limited);
        auto unavailable = (*limited)->registerStorage<TextureStorage>();
        CHECK(!unavailable && unavailable.error().type == kStorageCapability);
        CHECK((*limited)->stats().validation_errors == 0);
        // A type cannot silently rebuild a Session to enable missing device features.
    }
    RenderServerConfig config{"test.storage.registration", true};
    config.device.texture_descriptors = true;
    auto created = RenderServer::create(config);
    CHECK(created);
    auto& server = **created;
    auto registration = nativeStorageBinding<TextureStorage>();
    auto missing = registration;
    missing.factory.create = nullptr;
    CHECK((!server.registerStorage<SetTexture, SetTextureRegion, ReleaseTexture, QueryTexture>(missing)));
    auto mismatch = registration;
    auto wrong_description = TextureStorage::description;
    wrong_description.abi.minor = 1;
    mismatch.factory.description = &wrong_description;
    CHECK((!server.registerStorage<SetTexture, SetTextureRegion, ReleaseTexture, QueryTexture>(mismatch)));
    auto bad_handler = registration;
    CommandRegistration handler{static_cast<const CommandHandler&>(registration.handlers[0])};
    bad_handler.handlers = {&handler, 1};
    CHECK(!server.registerStorage<SetTexture>(bad_handler));
    CHECK(server.registerStorage<TextureStorage>());
    CHECK(!server.registerStorage<TextureStorage>());
    // The device exists before installation. Registration creates no storage instance or upload backing.
    // Real upload/region/release and two-client Manifest assertions live in texture_operations.
    CHECK((!server.registerStorage<SetTexture, SetTextureRegion, ReleaseTexture, QueryTexture>(mismatch)));
    return 0;
}
