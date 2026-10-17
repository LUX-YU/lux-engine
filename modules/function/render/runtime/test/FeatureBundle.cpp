#include "Discovery.hpp"
#include "../../native/vulkan/test/Support.hpp"
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/render/native/vulkan/mesh/MeshCommands.hpp>
#include <lux/engine/render/resource/Mesh.hpp>

using namespace foundation_test;
static unsigned constructed = 0, destroyed = 0;

// Test lifetime instrumentation delegates to the real Texture implementation, not a fake resource owner.
struct TrackedTexture final
{
    using Config = TextureStorageConfig;
    using Ops = lux::render::TextureStorage::Ops;
    static inline const auto description = lux::render::TextureStorage::description;
};
struct WrongTexture final
{
    using Config = TextureStorageConfig;
    using Ops = lux::render::TextureStorage::Ops;
    static inline const auto description = lux::render::TextureStorage::description;
};

struct TrackedHandler final
{
    static constexpr std::string_view name = "test.bundle.texture";
    vulkan::TextureCommands actual;
    explicit TrackedHandler(DeviceSession& session) noexcept : actual(session) { ++constructed; }
    ~TrackedHandler() noexcept { ++destroyed; }
    RenderResult<void> query(const QueryTexture::Request& input, Reply<QueryTexture>& reply) noexcept
    { return actual.query(input, reply); }
    using Bindings = std::tuple<CommandMethod<QueryTexture, &TrackedHandler::query>>;
};
struct WrongQuery final
{
    static constexpr std::string_view name = QueryTexture::name;
    using Request = unsigned;
    using Response = Ack;
};
struct WrongHandler final
{
    static constexpr std::string_view name = "test.bundle.wrong";
    explicit WrongHandler(DeviceSession&) noexcept {}
    RenderResult<void> query(const unsigned&, Reply<WrongQuery>& reply) noexcept { return reply.complete(Ack{}); }
    using Bindings = std::tuple<CommandMethod<WrongQuery, &WrongHandler::query>>;
};

namespace lux::render::vulkan
{
    template <> struct StorageProvider<::TrackedTexture> final
    {
        using Handler = ::TrackedHandler;
        static StorageFactory factory() noexcept { return StorageProvider<render::TextureStorage>::factory(); }
        static StorageCppType implementationType() noexcept
        { return StorageProvider<render::TextureStorage>::implementationType(); }
    };
    template <> struct StorageProvider<::WrongTexture> final
    {
        using Handler = ::WrongHandler;
        static StorageFactory factory() noexcept { return StorageProvider<render::TextureStorage>::factory(); }
        static StorageCppType implementationType() noexcept
        { return StorageProvider<render::TextureStorage>::implementationType(); }
    };
}

static RenderResult<void> describe(const void*, std::string_view, ResourceClaims&) noexcept { return {}; }
template <unsigned Mode> struct Bundle final
{
    using Handler = std::conditional_t<Mode == 5, TrackedHandler, void>;
    using Storages = std::conditional_t<Mode == 2, std::tuple<TrackedTexture, WrongTexture>,
        std::tuple<lux::render::MeshStorage, TrackedTexture, TrackedTexture>>;
    feature_detail::Implementation definition() const noexcept
    {
        static const FeatureDescriptor descriptor{FeatureName("test.bundle"), {}};
        static const FeatureGraphOps graph{describe, nullptr};
        return {Mode == 1 ? nullptr : &descriptor, &graph, nullptr};
    }
};

template <unsigned Mode> static void exercise()
{
    const auto original_constructed = constructed, original_destroyed = destroyed;
    {
        RenderServerConfig config{"test.bundle." + std::to_string(Mode), true};
        config.device.texture_descriptors = Mode != 0;
        auto server = take(RenderServer::create(config));
        if constexpr (Mode == 4)
        {
            TextureStorageConfig explicit_config;
            explicit_config.resident_versions_budget = 16;
            CHECK(server->registerStorage<TrackedTexture>(explicit_config, "custom"));
        }
        auto installed = server->registerFeature<Bundle<Mode>>(Bundle<Mode>{});
        if constexpr (Mode < 3)
        {
            CHECK(!installed);
            if constexpr (Mode == 0) CHECK(installed.error().type == kStorageCapability);
            if constexpr (Mode == 1) CHECK(installed.error().type == kFeatureContract);
            if constexpr (Mode == 2) CHECK(installed.error().type == kProtocolContract);
            CHECK(constructed - original_constructed == 1 && destroyed - original_destroyed == 1);
        }
        else
        {
            CHECK(installed);
            CHECK(take(server->registerFeature<Bundle<Mode>>(Bundle<Mode>{})) == *installed);
            CHECK(constructed - original_constructed == (Mode == 4 ? 2u : 1u));
            CHECK(destroyed == original_destroyed);
            if constexpr (Mode == 4)
            {
                TextureStorageConfig explicit_config;
                explicit_config.resident_versions_budget = 16;
                CHECK(server->registerStorage<TrackedTexture>(explicit_config, "custom"));
                CHECK(!server->registerStorage<TrackedTexture>({}, "custom"));
            }
        }
        bool run_ok = false;
        const auto directory_id = take(server->commandId<ListCommands>());
        std::thread thread([&] { run_ok = bool(server->run()); });
        auto client = take(runtime_test::connect(config.channel_name, directory_id));
        CHECK(runtime_test::refresh(*client));
        auto storages = take(runtime_test::coldCall<ListStorages>(*client, {}));
        auto features = take(runtime_test::coldCall<ListFeatures>(*client, {}));
        if constexpr (Mode < 3)
        {
            CHECK(storages.storages.empty() && features.features.empty());
            CHECK(!runtime_test::find<QueryTexture>(*client) && !runtime_test::find<QueryMesh>(*client));
        }
        else
        {
            CHECK(storages.storages.size() == 2 && features.features.size() == 1);
            CHECK(runtime_test::find<QueryMesh>(*client));
            const auto query = take(runtime_test::find<QueryTexture>(*client));
            auto choices = take(runtime_test::coldCall<ListCommandBindings>(*client, query));
            CHECK(choices.handlers.size() == (Mode == 4 ? 2u : 1u));
            const auto selected = std::ranges::find(choices.handlers, choices.selected, &HandlerInfo::id);
            CHECK(selected != choices.handlers.end());
            CHECK(selected->instance == (Mode == 4 ? "custom" : ""));
        }
        server->stop();
        thread.join();
        CHECK(run_ok && server->stats().validation_errors == 0);
    }
    CHECK(constructed - original_constructed == destroyed - original_destroyed);
}

int main()
{
    exercise<0>(); exercise<1>(); exercise<2>(); exercise<3>(); exercise<4>(); exercise<5>();
    std::puts("Feature support bundle: atomic failure, shared Handler, idempotence and explicit configuration retained");
}
