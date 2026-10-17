#include "Discovery.hpp"
#include <lux/engine/render/resource/Factory.hpp>
#include <lux/engine/render/data/ConfigValue.hpp>
#include <lux/engine/render/runtime/RenderServer.hpp>
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
                    result.required_capabilities = 0;
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
        if (mode == 2) config.resident_versions_budget = 16;
        const auto key = mode == 0 ? "" : (mode == 1 ? "second" : "failed");
        auto installed = server.registerStorage<TextureStorage>(config, key, code_pin);
        const auto completed = installed ? reply.complete(*installed) : reply.fail(installed.error());
        if (!completed) std::abort();
    }

    using Bindings = std::tuple<CommandMethod<InstallTextures, &Installer::install>>;
};

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
    auto input = runtime_test::connect(config.channel_name, *server.commandId<ListCommands>());
    CHECK(input);
    auto client = std::move(*input);
    bool run_ok = false;
    std::thread owner([&] { run_ok = bool(server.run()); });
    CHECK(runtime_test::refresh(*client));
    EmptyRequest empty;
    const auto listing = *runtime_test::find<ListStorages>(*client);
    auto initial = runtime_test::coldCall<ListStorages>(*client, empty);
    CHECK(initial && initial->storages.empty());
    CHECK(!runtime_test::find<QueryTexture>(*client)); // Failed factories did not publish any commands.
    const auto installing = *runtime_test::find<InstallTextures>(*client);
    unsigned mode = 0;
    auto installed = runtime_test::coldCall<InstallTextures>(*client, mode);
    CHECK(installed && !installed->isNull());
    auto listed = runtime_test::coldCall<ListStorages>(*client, empty);
    CHECK(listed && listed->storages.size() == 1);
    const auto& storage = listed->storages.front();
    CHECK(storage.id == *installed && storage.name == TextureStorage::description.type.name());
    CHECK((storage.capabilities & static_cast<unsigned>(EStorageCapability::SAMPLED_IMAGE_ARRAY)) != 0);
    CHECK((storage.capabilities & static_cast<unsigned>(EStorageCapability::BLOCK_COMPRESSION_BC)) == 0);
    CHECK(!runtime_test::find<QueryTexture>(*client)); // Discovery is explicitly refreshed, not broadcast.
    CHECK(runtime_test::refresh(*client));
    CHECK(runtime_test::find<QueryTexture>(*client));
    auto repeated = runtime_test::coldCall<InstallTextures>(*client, mode);
    CHECK(repeated && *repeated == storage.id);
    const auto query = *runtime_test::find<QueryTexture>(*client);
    const auto bindings = *runtime_test::find<ListCommandBindings>(*client);
    auto command_id = query;
    auto before = runtime_test::coldCall<ListCommandBindings>(*client, command_id);
    CHECK(before && before->handlers.size() == 1);
    const auto selected = before->selected;
    CHECK(!selected.isNull());
    mode = 1;
    auto second = runtime_test::coldCall<InstallTextures>(*client, mode);
    CHECK(second && *second == storage.id);
    auto after = runtime_test::coldCall<ListCommandBindings>(*client, command_id);
    CHECK(after && after->handlers.size() == 2 && after->selected == selected);
    CHECK(after->handlers.back().instance == "second");
    mode = 2;
    auto mismatch = runtime_test::coldCall<InstallTextures>(*client, mode);
    CHECK(!mismatch && mismatch.error().type == kStorageMismatch);
    auto retained = runtime_test::coldCall<ListCommandBindings>(*client, command_id);
    CHECK(retained && retained->handlers.size() == 2 && retained->selected == selected);
    auto unchanged = runtime_test::coldCall<ListStorages>(*client, empty);
    CHECK(unchanged && unchanged->storages.size() == 1);
    CHECK(unchanged->storages.front().id == storage.id);
    {
        // Real protocol controls resolve actual owners; no Domain ancestry or parallel instance table.
        const auto scene_a = runtime_test::coldCall<CreateScene>(*client, {});
        const auto scene_b = runtime_test::coldCall<CreateScene>(*client, {});
        CHECK(scene_a && scene_b);
        const auto view_a = runtime_test::coldCall<CreateView>(*client, {scene_a->scene, {}});
        CHECK(view_a);
        const std::array<VStorageOwner, 4> owners{
            GlobalStorageOwner{}, scene_a->scene, scene_b->scene, ViewRequest{scene_a->scene, view_a->view}
        };
        std::array<StorageId, 4> identities;
        for (std::size_t i = 0; i < owners.size(); ++i)
        {
            StorageLocation location{owners[i], storage.id, "prepared"};
            auto missing = runtime_test::coldCall<FindStorage>(*client, location);
            CHECK(!missing && missing.error().type == kStorageIdentity);
            auto prepared = runtime_test::coldCall<PrepareStorage>(*client, {location, {}});
            CHECK(prepared);
            identities[i] = *prepared;
            for (std::size_t j = 0; j < i; ++j) CHECK(identities[i] != identities[j]);
            auto found = runtime_test::coldCall<FindStorage>(*client, location);
            CHECK(found && *found == identities[i]);
            auto repeated = runtime_test::coldCall<PrepareStorage>(*client, {location, {}});
            CHECK(repeated && *repeated == identities[i]);
            CHECK(runtime_test::coldCall<ReleasePreparedStorage>(*client, identities[i]));
            CHECK(runtime_test::coldCall<FindStorage>(*client, location)); // the other preload remains
        }
        StorageLocation invalid{scene_a->scene, storage.id, "rejected"};
        auto wrong_config = ConfigValue::make(17u);
        CHECK(wrong_config);
        CHECK(!runtime_test::coldCall<PrepareStorage>(*client, {invalid, std::move(*wrong_config)}));
        CHECK(!runtime_test::coldCall<FindStorage>(*client, invalid));
        CHECK(runtime_test::coldCall<RemoveScene>(*client, {scene_a->scene}));
        CHECK(!runtime_test::coldCall<FindStorage>(*client, {owners[1], storage.id, "prepared"}));
        CHECK(!runtime_test::coldCall<FindStorage>(*client, {owners[3], storage.id, "prepared"}));
        CHECK(!runtime_test::coldCall<ReleasePreparedStorage>(*client, identities[1]));
        CHECK(!runtime_test::coldCall<ReleasePreparedStorage>(*client, identities[3]));
        for (const auto i : {0u, 2u})
        {
            CHECK(runtime_test::coldCall<FindStorage>(*client, {owners[i], storage.id, "prepared"}));
            CHECK(runtime_test::coldCall<ReleasePreparedStorage>(*client, identities[i]));
            CHECK(!runtime_test::coldCall<ReleasePreparedStorage>(*client, identities[i]));
        }
        const auto replacement = runtime_test::coldCall<CreateScene>(*client, {});
        CHECK(replacement && replacement->scene != scene_a->scene);
        CHECK(!runtime_test::coldCall<PrepareStorage>(*client, {{scene_a->scene, storage.id, "stale"}, {}}));
        CHECK(runtime_test::coldCall<RemoveScene>(*client, {scene_b->scene}));
        CHECK(runtime_test::coldCall<RemoveScene>(*client, {replacement->scene}));
    }
    server.stop();
    owner.join();
    CHECK(run_ok && server.stats().validation_errors == 0);
    created->reset();
    // Directory values own their strings and survive the Server; no message carries a module pin.
    CHECK(pin_releases == 1);
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
