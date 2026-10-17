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

// Malformed implementation providers exercise the last cold transaction boundary; application code
// cannot hand-assemble a Storage registration or pair unrelated erased handler pointers.
template <unsigned Fault> struct BrokenStorage final
{
    using Config = TextureStorageConfig;
    using Ops = TextureStorage::Ops;
    static inline constexpr auto description = TextureStorage::description;
};

namespace lux::render::vulkan
{
    template <unsigned Fault> struct StorageProvider<::BrokenStorage<Fault>> final
    {
        using Storage = TextureStorage;
        using Handler = TextureCommands;
        static StorageCppType implementationType() noexcept
        { return StorageProvider<render::TextureStorage>::implementationType(); }
        static StorageFactory factory() noexcept
        {
            auto value = StorageProvider<render::TextureStorage>::factory();
            if constexpr (Fault == 0) value.create = nullptr;
            if constexpr (Fault == 1) value.config_type = storageCppType<unsigned>();
            if constexpr (Fault == 2) value.ops_type = storageCppType<unsigned>();
            if constexpr (Fault == 3) value.native_type = storageCppType<unsigned>();
            if constexpr (Fault == 4)
            {
                static const auto wrong = [] {
                    auto result = render::TextureStorage::description;
                    ++result.abi.minor;
                    return result;
                }();
                value.description = &wrong;
            }
            return value;
        }
    };
}

template <class S> concept AcceptsErasedFactory = requires(S& server, StorageFactory factory) {
    server.registerStorage(factory);
};
static_assert(!AcceptsErasedFactory<RenderServer>);

using TextureBindings = vulkan::TextureCommands::Bindings;
static_assert(std::tuple_size_v<TextureBindings> == 4);
static_assert(std::same_as<std::tuple_element_t<0, TextureBindings>::Command, SetTexture>);
static_assert(std::same_as<std::tuple_element_t<1, TextureBindings>::Command, SetTextureRegion>);
static_assert(std::same_as<std::tuple_element_t<2, TextureBindings>::Command, ReleaseTexture>);
static_assert(std::same_as<std::tuple_element_t<3, TextureBindings>::Command, QueryTexture>);

struct InstallTextures final
{
    static constexpr std::string_view name = "test.install.textures";
    using Request = unsigned;
    using Response = StorageTypeId;
};

struct Installer final
{
    static constexpr std::string_view name = "test.storage.installer";
    RenderServer& server;
    std::shared_ptr<const void> code_pin;

    Installer(RenderServer& value, std::shared_ptr<const void> pin) noexcept
        : server(value), code_pin(std::move(pin)) {}

    void install(const unsigned& mode, Reply<InstallTextures> reply) noexcept
    {
        TextureStorageConfig config;
        if (mode == 2) config.capacity *= 2;
        const auto key = mode == 0 ? "" : (mode == 1 ? "second" : "failed");
        auto installed = server.registerStorage<TextureStorage>(config, key, code_pin);
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

static int exerciseRegistration(unsigned& pin_releases)
{
    auto pin = std::shared_ptr<const void>(new unsigned{}, [&pin_releases](const void* value) noexcept {
        delete static_cast<const unsigned*>(value);
        ++pin_releases;
    });
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
    CHECK(!server.registerStorage<BrokenStorage<0>>());
    CHECK(!server.registerStorage<BrokenStorage<1>>());
    CHECK(!server.registerStorage<BrokenStorage<2>>());
    CHECK(!server.registerStorage<BrokenStorage<3>>());
    CHECK(!server.registerStorage<BrokenStorage<4>>());
    CHECK(server.emplaceHandler<Installer>("", {}, server, pin));
    pin.reset();
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
    unsigned mode = 0;
    auto installed = receive(*client, *client->submit(installing, mode)).take(installing);
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
    auto repeated = receive(*client, *client->submit(installing, mode)).take(installing);
    CHECK(repeated && repeated->response() == storage.id);
    const auto query = *client->bind<QueryTexture>();
    const auto bindings = *client->bind<ListCommandBindings>();
    auto command_id = query.command();
    auto before = receive(*client, *client->submit(bindings, command_id, 8192)).take(bindings);
    CHECK(before && before->response().handlers.size() == 1);
    const auto selected = before->response().selected;
    CHECK(!selected.isNull());
    mode = 1;
    auto second = receive(*client, *client->submit(installing, mode)).take(installing);
    CHECK(second && second->response() == storage.id);
    auto after = receive(*client, *client->submit(bindings, command_id, 8192)).take(bindings);
    CHECK(after && after->response().handlers.size() == 2 && after->response().selected == selected);
    CHECK(after->response().handlers.back().instance == "second");
    mode = 2;
    auto mismatch = receive(*client, *client->submit(installing, mode));
    CHECK(mismatch.header.outcome == ECommandOutcome::FAILED && mismatch.header.error.type == kStorageMismatch);
    auto retained = receive(*client, *client->submit(bindings, command_id, 8192)).take(bindings);
    CHECK(retained && retained->response().handlers.size() == 2 && retained->response().selected == selected);
    auto unchanged = receive(*client, *client->submit(listing, empty, 8192)).take(listing);
    CHECK(unchanged && unchanged->response().storages.size() == 1);
    CHECK(unchanged->response().storages.front().id == storage.id);
    server.stop();
    owner.join();
    CHECK(run_ok && server.stats().validation_errors == 0);
    created->reset();
    // The refreshed canonical type operations and directory response outlive the Server.
    CHECK(pin_releases == 0);
    CHECK(storage.name == TextureStorage::description.type.name());
    return 0;
}

int main()
{
    unsigned pin_releases = 0;
    CHECK(exerciseRegistration(pin_releases) == 0);
    CHECK(pin_releases == 1);
    return 0;
}
