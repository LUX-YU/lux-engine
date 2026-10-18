#include "../../resource/test/FactoryAudit.hpp"
#include "ServerFixture.hpp"
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

namespace lux::render
{
    template <unsigned Fault> struct StorageProvider<::BrokenStorage<Fault>> final
    {
        using Storage = vulkan::TextureStorage;
        using Handler = vulkan::TextureCommandHandler;
        static std::unique_ptr<Handler> createHandler(vulkan::DeviceSession& device) noexcept
        { return std::make_unique<Handler>(device); }
        static CppType implementationType() noexcept
        { return StorageProvider<render::TextureStorage>::implementationType(); }
        static StorageFactory factory() noexcept
        {
            const auto value = StorageProvider<render::TextureStorage>::factory();
            static const auto wrong = [] {
                auto result = render::TextureStorage::description;
                result.required_capabilities = 0;
                return result;
            }();
            return resource_detail::StorageFactoryAudit::make(
                Fault == 4 ? &wrong : value.description, value.ops,
                Fault == 0 ? nullptr : value.create, value.compatible_config, value.binding_ops,
                Fault == 1 ? cppType<unsigned>() : value.config_type,
                Fault == 2 ? cppType<unsigned>() : value.ops_type,
                Fault == 3 ? cppType<unsigned>() : value.native_type);
        }
    };
}

template <class S> concept AcceptsErasedFactory = requires(S& server, StorageFactory factory) {
    server.registerStorage(factory);
};
static_assert(!AcceptsErasedFactory<RenderServer>);

using TextureBindings = vulkan::TextureCommandHandler::Bindings;
static_assert(std::tuple_size_v<TextureBindings> == 4);
static_assert(std::same_as<std::tuple_element_t<0, TextureBindings>::Command, SetTexture>);
static_assert(std::same_as<std::tuple_element_t<1, TextureBindings>::Command, SetTextureRegion>);
static_assert(std::same_as<std::tuple_element_t<2, TextureBindings>::Command, ReleaseTexture>);
static_assert(std::same_as<std::tuple_element_t<3, TextureBindings>::Command, QueryTexture>);

struct InstallTextures final
{
    static constexpr std::string_view name = "test.install.textures";
    using Request = unsigned;
    using Response = RenderResult<StorageTypeId>;
};

struct Installer final
{
    static constexpr std::string_view name = "test.storage.installer";
    RenderServer& server;
    unsigned& destroyed;

    Installer(RenderServer& value, unsigned& count) noexcept : server(value), destroyed(count) {}
    ~Installer() noexcept { ++destroyed; }

    void install(const unsigned& mode, InstallTextures::Response& reply) noexcept
    {
        TextureStorageConfig config;
        if (mode == 2) config.resident_versions_budget = 16;
        const auto key = mode == 0 ? "" : (mode == 1 ? "second" : "failed");
        auto installed = server.registerStorage<TextureStorage>(config, key);
        reply = std::move(installed);
    }

    using Bindings = std::tuple<CommandMethod<InstallTextures, &Installer::install>>;
};

static int exerciseRegistration(unsigned& handler_destructions)
{
    {
        auto limited = createTestServer({"test.storage.capabilities", true});
        CHECK(limited);
        auto unavailable = (*limited)->registerStorage<TextureStorage>();
        CHECK(!unavailable && unavailable.error().type == kStorageCapability);
        CHECK((*limited)->stats().validation_errors == 0);
        // A type cannot silently rebuild a Session to enable missing device features.
    }
    RenderServerConfig config{"test.storage.registration", true};
    vulkan::DeviceOptions config_device{.dynamic_rendering = true};
    config_device.texture_descriptors = true;
    auto created = createTestServer(config, config_device);
    CHECK(created);
    auto& server = **created;
    CHECK(!server.registerStorage<BrokenStorage<0>>());
    CHECK(!server.registerStorage<BrokenStorage<1>>());
    CHECK(!server.registerStorage<BrokenStorage<2>>());
    CHECK(!server.registerStorage<BrokenStorage<3>>());
    CHECK(!server.registerStorage<BrokenStorage<4>>());
    CHECK(server.emplaceHandler<Installer>("", server, handler_destructions));
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
        const auto view_a = runtime_test::coldCall<CreateView>(*client, {scene_a->scene});
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
    CHECK(handler_destructions == 1);
    CHECK(storage.name == TextureStorage::description.type.name());
    return 0;
}

int main()
{
    unsigned handler_destructions = 0;
    CHECK(exerciseRegistration(handler_destructions) == 0);
    CHECK(handler_destructions == 1);
    return 0;
}
