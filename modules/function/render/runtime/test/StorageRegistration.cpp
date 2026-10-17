#include <lux/engine/render/resource/Registration.hpp>
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/render/runtime/RenderClient.hpp>
#include <lux/engine/render/runtime/Commands.hpp>
#include <lux/engine/render/resource/Texture.hpp>

#include <cstdio>
#include <thread>
#include <chrono>

using namespace lux::render;

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #value); return 1; } } while (false)

struct InstallTextures final
{
    static constexpr std::string_view name = "test.install.textures";
    using Request = EmptyRequest;
    using Response = StorageTypeId;
};

struct Installer final
{
    static constexpr std::string_view name = "test.storage.installer";
    RenderServer& server;
    explicit Installer(RenderServer& value) noexcept : server(value) {}

    void install(const EmptyRequest&, Reply<InstallTextures> reply) noexcept
    {
        auto installed = server.registerStorage<TextureStorage>();
        const auto completed = installed ? reply.complete(*installed) : reply.fail(installed.error());
        if (!completed) std::abort();
    }

    using Bindings = std::tuple<CommandMethod<InstallTextures, &Installer::install>>;
};

static RenderCompletion receive(RenderClient& client, RequestId id)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline)
    {
        RenderCompletion value;
        auto received = client.tryReceive(value);
        if (!received || *received == ERenderReceive::CLOSED) std::abort();
        if (*received == ERenderReceive::RECEIVED)
        {
            if (value.header.request != id) std::abort();
            return value;
        }
        std::this_thread::yield();
    }
    std::abort();
}

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
    struct WrongConfig { std::uint32_t value; };
    auto wrong_config = registration;
    wrong_config.configuration = *ConfigValue::make(WrongConfig{17});
    CHECK((!server.registerStorage<SetTexture, SetTextureRegion, ReleaseTexture, QueryTexture>(wrong_config)));
    CHECK((!server.registerStorage<SetTexture, SetTextureRegion, ReleaseTexture, QueryTexture>(mismatch)));
    CHECK(server.emplaceHandler<Installer>("", {}, server));
    auto input = RenderClient::create(config.channel_name);
    CHECK(input);
    auto client = std::move(*input);
    bool run_ok = false;
    std::thread owner([&] { run_ok = bool(server.run()); });
    receive(*client, *client->hello());
    EmptyRequest empty;
    const auto listing = *client->bind<ListStorages>();
    auto initial = receive(*client, *client->submit(listing, empty, 8192)).take(listing);
    CHECK(initial && initial->response().storages.empty());
    CHECK(!client->bind<QueryTexture>()); // Failed factories did not publish any commands.
    const auto installing = *client->bind<InstallTextures>();
    auto installed = receive(*client, *client->submit(installing, empty)).take(installing);
    CHECK(installed && !installed->response().isNull());
    auto listed = receive(*client, *client->submit(listing, empty, 8192)).take(listing);
    CHECK(listed && listed->response().storages.size() == 1);
    const auto& storage = listed->response().storages.front();
    CHECK(storage.id == installed->response() && storage.name == TextureStorage::description.type.name());
    CHECK((storage.capabilities & static_cast<unsigned>(EStorageCapability::SAMPLED_IMAGE_ARRAY)) != 0);
    CHECK((storage.capabilities & static_cast<unsigned>(EStorageCapability::BLOCK_COMPRESSION_BC)) == 0);
    CHECK(!client->bind<QueryTexture>()); // Discovery is explicitly refreshed, not broadcast.
    const auto commands = *client->bind<ListCommands>();
    auto refreshed = receive(*client, *client->submit(commands, empty, 32768));
    CHECK(client->refreshCommands(refreshed));
    CHECK(client->bind<QueryTexture>());
    auto duplicate = receive(*client, *client->submit(installing, empty));
    CHECK(duplicate.header.outcome == ECommandOutcome::FAILED);
    auto unchanged = receive(*client, *client->submit(listing, empty, 8192)).take(listing);
    CHECK(unchanged && unchanged->response().storages.size() == 1);
    CHECK(unchanged->response().storages.front().id == storage.id);
    server.stop();
    owner.join();
    CHECK(run_ok && server.stats().validation_errors == 0);
    created->reset();
    CHECK(storage.name == TextureStorage::description.type.name());
    return 0;
}
