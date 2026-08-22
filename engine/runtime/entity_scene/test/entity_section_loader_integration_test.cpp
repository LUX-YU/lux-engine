#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/ecs/PersistentEntityIndex.hpp>
#include <lux/engine/ecs/Schedule.hpp>
#include <lux/engine/ecs/SystemPhase.hpp>
#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/components/ParentComponent.hpp>
#include <lux/engine/ecs/scene_format/EntitySectionCodec.hpp>
#include <lux/engine/scene/SceneDescription.hpp>
#include <lux/engine/runtime/entity_scene/EntitySceneCatalog.hpp>
#include <lux/engine/runtime/entity_scene/EntitySectionGeneratorCatalog.hpp>
#include <lux/engine/runtime/entity_scene/EntitySectionService.hpp>
#include <lux/engine/runtime/entity_scene/SectionBlobStore.hpp>
#include <lux/engine/ecs/entity_scene/EntitySectionLoaderSystem.hpp>
#include <lux/engine/ecs/entity_scene/StartupSectionSystem.hpp>
#include <lux/engine/runtime/execution/AsyncRuntime.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeSenders.hpp>
#include <lux/engine/runtime/execution/AsyncScopeSenders.hpp>
#include <lux/engine/runtime/execution/testing/AsyncCloseTestDriver.hpp>

#include <lux/cxx/algorithm/hash.hpp>

#include <exec/start_detached.hpp>
#include <stdexec/execution.hpp>

#include <algorithm>
#include <atomic>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    enum class EGateError : std::uint8_t
    {
        CLOSED
    };

    struct CoordinatorGateState final
    {
        std::atomic<bool> entered{false};
        std::atomic<bool> release{false};
    };

    struct HoldCoordinator final
    {
        using Value = void;
        using Error = EGateError;

        std::shared_ptr<CoordinatorGateState> state;
    };

    struct MarkerComponent final
    {
        // This test exercises value-component transfer. Empty EnTT types are
        // tags and intentionally have no address returned by try_get().
        std::uint8_t value{0u};
    };

    uuids::uuid uuid(const char* value)
    {
        return uuids::uuid::from_string(value).value();
    }

    uuids::uuid ordinalUuid(std::uint64_t ordinal)
    {
        std::array<std::uint8_t, 16u> bytes{};
        bytes[6] = 0x40u;
        bytes[8] = 0x80u;
        for (std::size_t index = 0u; index < 8u; ++index)
        {
            bytes[15u - index] = static_cast<std::uint8_t>(
                ordinal >> (index * 8u));
        }
        return uuids::uuid{bytes};
    }

    template<class Component>
    bool has(
        lux::ecs::RegistryBase& registry,
        entt::entity entity)
    {
        return registry.all_of<Component>(entity);
    }

    template<class Component>
    void* get(
        lux::ecs::RegistryBase& registry,
        entt::entity entity)
    {
        return registry.try_get<Component>(entity);
    }

    template<class Component>
    void* emplace(
        lux::ecs::RegistryBase& registry,
        entt::entity entity)
    {
        return &registry.emplace_or_replace<Component>(entity);
    }

    template<class Component>
    void remove(
        lux::ecs::RegistryBase& registry,
        entt::entity entity)
    {
        static_cast<void>(registry.remove<Component>(entity));
    }

    template<class Component>
    void notify(
        lux::ecs::RegistryBase& registry,
        entt::entity entity)
    {
        if (registry.all_of<Component>(entity))
            registry.patch<Component>(entity);
    }

    template<class Component>
    void reserve(
        lux::ecs::RegistryBase& registry,
        std::size_t additional)
    {
        auto& storage = registry.storage<Component>();
        storage.reserve(storage.size() + additional);
    }

    template<class Component>
    void* transfer(
        lux::ecs::RegistryBase& source,
        entt::entity source_entity,
        lux::ecs::RegistryBase& destination,
        entt::entity destination_entity) noexcept
    {
        if (!source.all_of<Component>(source_entity))
            return nullptr;
        auto* value = &destination.emplace<Component>(destination_entity);
        static_cast<void>(source.remove<Component>(source_entity));
        return value;
    }

    void registerMarker(lux::ecs::ComponentTypeCatalog& catalog)
    {
        constexpr std::string_view schema = "org.lux.test.marker";
        const auto type = lux::cxx::typeToken<MarkerComponent>();
        const auto added = catalog.registerSchema(
            lux::ecs::ComponentSchemaDescriptor{
                type,
                {lux::cxx::algorithm::fnv1a(schema), std::string{schema}},
                1u,
                nullptr,
                {&has<MarkerComponent>,
                 &get<MarkerComponent>,
                 &emplace<MarkerComponent>,
                 &remove<MarkerComponent>,
                 nullptr,
                 &notify<MarkerComponent>,
                 &reserve<MarkerComponent>,
                 &transfer<MarkerComponent>,
                 true},
                "org.lux.test",
                {},
                lux::ecs::EComponentSerializationPolicy::COOKED});
        assert(added);
    }

    struct SectionFixture final
    {
        lux::ecs::scene_format::SectionRecord record;
        lux::asset::asset_id_t asset;
        std::vector<std::byte> bytes;
    };

    lux::ecs::scene_format::EntitySectionImage makeMarkerImage(
        lux::ecs::scene_format::EntitySectionId section)
    {
        using namespace lux::ecs::scene_format;
        EntitySectionImage image;
        image.section = section;
        image.component_names = {""};
        image.schemas.push_back({
            lux::ecs::componentSchemaId("org.lux.test.marker"),
            1u,
            EEntityComponentStorage::TAG});
        image.archetypes.push_back({{0u}});
        image.entities = {{0u, std::nullopt}, {0u, std::nullopt}};
        image.parents.push_back({1u, 0u});
        return image;
    }

    lux::cxx::expected<
        lux::ecs::scene_format::EntitySectionImage,
        lux::runtime::entity_scene::EntitySectionGeneratorFailure>
    generateMarkerSection(
        const void*,
        lux::runtime::entity_scene::GeneratedEntitySectionRequest request)
        noexcept
    {
        return makeMarkerImage(request.record.id);
    }

    SectionFixture makeSection(
        const char* section_id,
        const char* asset_id,
        std::string path)
    {
        const lux::ecs::scene_format::EntitySectionId section{
            uuid(section_id)};
        auto image = makeMarkerImage(section);
        auto encoded =
            lux::ecs::scene_format::encodeEntitySectionImage(image);
        assert(encoded);

        lux::ecs::scene_format::SectionRecord record;
        record.id = section;
        record.source = lux::ecs::scene_format::StoredSectionSource{std::move(path)};
        record.content_digest =
            lux::ecs::scene_format::entitySectionContentDigest(*encoded);
        record.encoded_bytes = encoded->size();
        record.decoded_bytes = encoded->size();
        record.entity_count = 2u;
        return {std::move(record), uuid(asset_id), std::move(*encoded)};
    }

    class MemoryProvider final : public lux::asset::IAssetProvider
    {
    public:
        struct Entry final
        {
            std::string path;
            lux::asset::asset_id_t id;
            std::vector<std::byte> bytes;
            bool waits_for_peer{false};
            bool releases_peer{false};
        };

        explicit MemoryProvider(std::vector<Entry> entries)
            : entries_(std::move(entries))
        {}

        [[nodiscard]] std::optional<lux::asset::asset_id_t> resolve(
            std::string_view path) const override
        {
            for (const auto& entry : entries_)
            {
                if (entry.path == path)
                    return entry.id;
            }
            return std::nullopt;
        }

        [[nodiscard]] bool contains(
            const lux::asset::asset_id_t& id) const override
        {
            return find(id) != nullptr;
        }

        [[nodiscard]] lux::cxx::expected<
            lux::asset::AssetBlob,
            lux::asset::EAssetError>
        open(const lux::asset::asset_id_t& id) const override
        {
            open_count_.fetch_add(1u, std::memory_order_relaxed);
            const auto* entry = find(id);
            if (!entry)
            {
                return lux::cxx::unexpected(
                    lux::asset::EAssetError::ASSET_NOT_EXIST);
            }
            if (entry->releases_peer)
            {
                peer_released_.store(true, std::memory_order_release);
                peer_released_.notify_all();
            }
            if (entry->waits_for_peer)
            {
                while (!peer_released_.load(std::memory_order_acquire))
                    peer_released_.wait(false, std::memory_order_acquire);
            }
            auto owner = std::shared_ptr<std::byte[]>{
                new std::byte[entry->bytes.size()]};
            std::memcpy(
                owner.get(), entry->bytes.data(), entry->bytes.size());
            return lux::asset::AssetBlob::fromSharedArray(
                std::move(owner), entry->bytes.size());
        }

        void enumerate(
            const std::function<void(const lux::asset::ProviderEntry&)>& fn)
            const override
        {
            for (const auto& entry : entries_)
            {
                fn({
                    entry.id,
                    lux::ecs::scene_format::kEntitySectionImageMagic,
                    entry.path,
                    false});
            }
        }

        [[nodiscard]] std::optional<std::string> pathOf(
            const lux::asset::asset_id_t& id) const override
        {
            const auto* entry = find(id);
            return entry ? std::optional{entry->path} : std::nullopt;
        }

        [[nodiscard]] std::uint64_t openCount() const noexcept
        {
            return open_count_.load(std::memory_order_relaxed);
        }

    private:
        [[nodiscard]] const Entry* find(
            const lux::asset::asset_id_t& id) const noexcept
        {
            for (const auto& entry : entries_)
            {
                if (entry.id == id)
                    return &entry;
            }
            return nullptr;
        }

        std::vector<Entry> entries_;
        mutable std::atomic<bool> peer_released_{false};
        mutable std::atomic<std::uint64_t> open_count_{0u};
    };

    template<class Predicate>
    void drive(
        lux::exec::AsyncRuntime& runtime,
        lux::ecs::Schedule& first,
        lux::ecs::entity_scene::EntitySectionLoaderSystem& first_owner,
        lux::ecs::entity_scene::StartupSectionSystem* first_scene,
        lux::ecs::Schedule& second,
        lux::ecs::entity_scene::EntitySectionLoaderSystem& second_owner,
        Predicate&& done)
    {
        lux::exec::testing::CloseEpoch progress{runtime};
        progress.driveWithStep(
            [&]() noexcept
            {
                first.tick(0.0f);
                second.tick(0.0f);
            },
            done,
            [&first_owner, first_scene, &second_owner]() noexcept
            {
                const auto has_local_work = [](const auto& state) noexcept
                {
                    return state.waiting_admission_sections != 0u ||
                        state.staging_sections != 0u ||
                        state.armed_sections != 0u;
                };
                const auto first_state = first_owner.snapshot();
                const auto selector_observation_pending = first_scene &&
                    first_scene->state() == lux::ecs::entity_scene::
                        EEntitySceneState::LOADING &&
                    first_state.waiting_sections == 0u &&
                    first_state.staging_sections == 0u &&
                    first_state.armed_sections == 0u;
                return has_local_work(first_state) ||
                    has_local_work(second_owner.snapshot()) ||
                    selector_observation_pending;
            });
        assert(done());
    }

    void closeOwner(
        lux::ecs::entity_scene::EntitySectionLoaderSystem& owner,
        lux::exec::AsyncRuntime& runtime,
        lux::ecs::Schedule& schedule)
    {
        lux::exec::testing::CloseEpoch progress{runtime};
        const auto required_barrier =
            owner.snapshot().active_sections != 0u;
        owner.requestClose();
        static_cast<void>(runtime.drainMainThreadCompletions(256u));
        // When live rows existed, async work alone is insufficient: the close
        // sender retains one scope admission until the unique ECS barrier
        // destroys them. An already-empty owner may settle immediately.
        if (required_barrier)
            assert(!owner.closeComplete());
        progress.driveWithStep(
            [&schedule]() noexcept { schedule.tick(0.0f); },
            [&owner]() noexcept { return owner.closeComplete(); },
            [&owner]() noexcept
            {
                if (owner.closeComplete())
                    return false;
                const auto state = owner.snapshot();
                return state.waiting_admission_sections != 0u ||
                    state.staging_sections != 0u ||
                    state.armed_sections != 0u ||
                    state.active_sections != 0u;
            });
    }

    void closeScene(
        lux::ecs::entity_scene::StartupSectionSystem& scene,
        lux::exec::AsyncRuntime& runtime,
        lux::ecs::Schedule& schedule)
    {
        lux::exec::testing::CloseEpoch progress{runtime};
        scene.requestClose();
        static_cast<void>(runtime.drainMainThreadCompletions(256u));
        assert(!scene.closeComplete());
        progress.driveWithStep(
            [&schedule]() noexcept
            {
                schedule.tick(0.0f, lux::ecs::kPhaseSceneLoading);
            },
            [&scene]() noexcept { return scene.closeComplete(); },
            [&scene]() noexcept
            {
                return !scene.closeComplete() &&
                    scene.state() == lux::ecs::entity_scene::
                        EEntitySceneState::CLOSING;
            });
        assert(scene.state() ==
            lux::ecs::entity_scene::EEntitySceneState::CLOSED);
    }
}

int main()
{
    namespace runtime = lux::runtime::entity_scene;

    auto common = makeSection(
        "71000000-0000-4000-8000-000000000001",
        "72000000-0000-4000-8000-000000000001",
        "/Game/Scenes/Common_lxes");
    auto delayed = makeSection(
        "71000000-0000-4000-8000-000000000002",
        "72000000-0000-4000-8000-000000000002",
        "/Game/Scenes/Delayed_lxes");
    auto release = makeSection(
        "71000000-0000-4000-8000-000000000003",
        "72000000-0000-4000-8000-000000000003",
        "/Game/Scenes/Release_lxes");
    auto fixed_leaf = makeSection(
        "71000000-0000-4000-8000-000000000004",
        "72000000-0000-4000-8000-000000000004",
        "/Game/Scenes/FixedLeaf_lxes");
    fixed_leaf.record.dependencies.push_back(common.record.id);

    auto provider = std::make_shared<MemoryProvider>(
        std::vector<MemoryProvider::Entry>{
            {"Scenes/Common_lxes", common.asset, common.bytes},
            {"Scenes/Delayed_lxes", delayed.asset, delayed.bytes, true, false},
            {"Scenes/Release_lxes", release.asset, release.bytes, false, true},
            {"Scenes/FixedLeaf_lxes", fixed_leaf.asset, fixed_leaf.bytes}});
    auto vfs = std::make_shared<lux::asset::AssetVfs>();
    assert(vfs->mount({"/Game", provider, 0}) !=
        lux::asset::kInvalidMountId);

    lux::exec::AsyncRuntimeBuilder builder;
    auto generators = runtime::EntitySectionGeneratorCatalog::create({
        runtime::EntitySectionGeneratorDescriptor{
            lux::ecs::scene_format::SectionGeneratorId{
                "org.lux.test.generator"},
            &generateMarkerSection,
            {},
            {}}});
    assert(generators);
    auto service_result = runtime::EntitySectionService::addTo(
        builder, *generators);
    assert(service_result);
    auto gate_state = std::make_shared<CoordinatorGateState>();
    auto gate_operation = builder.addOperation<HoldCoordinator>(
        [](HoldCoordinator&& request,
           lux::exec::AsyncOperationContext&,
           lux::exec::AsyncOperationCompletion<HoldCoordinator>&& completion)
            noexcept
        {
            request.state->entered.store(true, std::memory_order_release);
            request.state->entered.notify_all();
            request.state->release.wait(false, std::memory_order_acquire);
            completion.complete({});
        });
    assert(gate_operation);
    auto plan = std::move(builder).compile();
    assert(plan);
    lux::exec::AsyncRuntime async(
        std::move(*plan),
        lux::exec::AsyncRuntimeConfig{
            .blocking_io_threads = 2u,
            .background_cpu_concurrency = 2u});
    auto service = std::move(*service_result);
    lux::exec::AsyncScope gate_scope{async};
    auto hold_coordinator =
        lux::exec::execute(*gate_operation, HoldCoordinator{gate_state})
        | stdexec::then(
              [](lux::async::OperationOutcome<HoldCoordinator>) noexcept {});
    assert(lux::exec::spawn(
        gate_scope,
        std::move(hold_coordinator)));
    gate_state->entered.wait(false, std::memory_order_acquire);

    lux::ecs::ComponentTypeCatalog components;
    registerMarker(components);

    lux::ecs::World fixed_world;
    lux::ecs::PersistentEntityIndex fixed_persistent_entities{
        fixed_world.registry()};
    lux::ecs::Schedule fixed_schedule{fixed_world};
    auto fixed_loader = std::make_unique<lux::ecs::entity_scene::EntitySectionLoaderSystem>(
        service.loadClient(),
        vfs,
        std::make_unique<runtime::SectionBlobStore>(),
        components,
        fixed_persistent_entities);
    auto* fixed_owner = fixed_loader.get();
    assert(fixed_schedule.addSystem(
        std::move(fixed_loader), lux::ecs::kPhaseSceneLoading));
    lux::scene::SceneDescription fixed_package;
    fixed_package.id = lux::asset::asset_id_t{
        uuid("70000000-0000-4000-8000-000000000001")};
    fixed_package.startup_sections.push_back(fixed_leaf.record.id);
    fixed_package.sections.push_back(common.record);
    fixed_package.sections.push_back(fixed_leaf.record);
    auto fixed_catalog_result = runtime::EntitySceneCatalog::create(
        std::move(fixed_package));
    assert(fixed_catalog_result);
    auto fixed_catalog = std::move(*fixed_catalog_result);
    const auto& fixed_scene_package = fixed_catalog.package();
    auto fixed_scene_result =
        lux::ecs::entity_scene::StartupSectionSystem::create(
            fixed_scene_package.id,
            fixed_scene_package.sections,
            fixed_scene_package.startup_sections,
            fixed_scene_package.required_components,
            *fixed_owner);
    assert(fixed_scene_result);
    auto fixed_scene = std::move(*fixed_scene_result);
    auto* fixed_scene_owner = fixed_scene.get();
    assert(fixed_scene_owner->state() ==
        lux::ecs::entity_scene::EEntitySceneState::LOADING);
    assert(fixed_scene_owner->revision() == 0u);
    assert(fixed_schedule.addSystem(
        std::move(fixed_scene), lux::ecs::kPhaseSceneLoading));
    assert(fixed_schedule.compile().valid());

    lux::ecs::World manual_world;
    lux::ecs::PersistentEntityIndex manual_persistent_entities{
        manual_world.registry()};
    lux::ecs::Schedule manual_schedule{manual_world};
    auto manual_loader = std::make_unique<lux::ecs::entity_scene::EntitySectionLoaderSystem>(
        service.loadClient(),
        vfs,
        std::make_unique<runtime::SectionBlobStore>(),
        components,
        manual_persistent_entities);
    auto* manual_owner = manual_loader.get();
    assert(manual_schedule.addSystem(
        std::move(manual_loader), lux::ecs::kPhaseSceneLoading));
    assert(manual_schedule.compile().valid());
    auto manual_ticket = manual_owner->client().acquire(common.record);
    assert(manual_ticket);
    const auto first_loader_generation = manual_ticket->generation();
    auto manual_leaf_ticket =
        manual_owner->client().acquire(fixed_leaf.record);
    assert(manual_leaf_ticket);
    // Constructing/validating the LXSC owner and its Schedule topology never
    // synchronously opens a Section. The coordinator gate keeps separately
    // submitted manual work from racing this observation.
    assert(provider->openCount() == 0u);

    // Hold the coordinator so the bounded EntitySection queue reaches a
    // deterministic admission boundary. The rejected ticket remains on the
    // same generation and update() retries it after capacity becomes free.
    std::vector<lux::ecs::entity_scene::EntitySectionTicket> saturated;
    saturated.reserve(
        lux::ecs::entity_scene::kEntitySectionLoadQueueCapacity + 4u);
    for (std::size_t index = 0u;
         index < lux::ecs::entity_scene::kEntitySectionLoadQueueCapacity + 4u;
         ++index)
    {
        auto record = common.record;
        record.id = lux::ecs::scene_format::EntitySectionId{
            ordinalUuid(10'000u + index)};
        auto ticket = manual_owner->client().acquire(std::move(record));
        assert(ticket);
        saturated.push_back(std::move(*ticket));
    }
    static_cast<void>(async.drainMainThreadCompletions(256u));
    auto retry = std::find_if(
        saturated.begin(), saturated.end(),
        [](const lux::ecs::entity_scene::EntitySectionTicket& ticket)
        {
            return ticket.state() ==
                lux::ecs::entity_scene::EEntitySectionState::WAITING_ADMISSION;
        });
    assert(retry != saturated.end());
    const auto retry_generation = retry->generation();
    lux::ecs::entity_scene::EntitySectionTicket retry_ticket = std::move(*retry);
    saturated.clear();
    gate_state->release.store(true, std::memory_order_release);
    gate_state->release.notify_all();
    lux::exec::testing::closeScope(gate_scope, async);

    drive(
        async,
        fixed_schedule,
        *fixed_owner,
        fixed_scene_owner,
        manual_schedule,
        *manual_owner,
        [&]() noexcept
        {
            return fixed_scene_owner->state() ==
                    lux::ecs::entity_scene::EEntitySceneState::READY &&
                manual_ticket->state() == lux::ecs::entity_scene::EEntitySectionState::ACTIVE &&
                manual_leaf_ticket->state() ==
                    lux::ecs::entity_scene::EEntitySectionState::ACTIVE &&
                retry_ticket.state() == lux::ecs::entity_scene::EEntitySectionState::FAILED;
        });
    assert(retry_ticket.generation() == retry_generation);
    assert(manual_owner->snapshot().queue_backpressure != 0u);
    assert(fixed_scene_owner->revision() == 1u);
    fixed_schedule.tick(0.0f, lux::ecs::kPhaseSceneLoading);
    assert(fixed_scene_owner->revision() == 1u);
    retry_ticket.reset();
    assert(fixed_world.registry().view<MarkerComponent>().size() == 4u);
    assert(manual_world.registry().view<MarkerComponent>().size() == 4u);
    assert(fixed_world.registry().view<lux::ecs::ParentComponent>().size() ==
        2u);
    assert(manual_world.registry().view<lux::ecs::ParentComponent>().size() ==
        2u);
    manual_leaf_ticket->reset();
    manual_schedule.tick(0.0f, lux::ecs::kPhaseSceneLoading);
    assert(manual_world.registry().view<MarkerComponent>().size() == 2u);

    // The delayed cancelled generation completes after its peer. It must be
    // discarded, then the same Section can be acquired with a new generation.
    auto cancelled = manual_owner->client().acquire(delayed.record);
    assert(cancelled);
    const auto cancelled_generation = cancelled->generation();
    cancelled->reset();
    auto release_ticket = manual_owner->client().acquire(release.record);
    assert(release_ticket);
    drive(
        async,
        fixed_schedule,
        *fixed_owner,
        fixed_scene_owner,
        manual_schedule,
        *manual_owner,
        [&]() noexcept
        {
            return release_ticket->state() ==
                    lux::ecs::entity_scene::EEntitySectionState::ACTIVE &&
                manual_owner->snapshot().stale_completions != 0u;
        });
    auto delayed_retry = manual_owner->client().acquire(delayed.record);
    assert(delayed_retry);
    assert(delayed_retry->generation() > cancelled_generation);
    drive(
        async,
        fixed_schedule,
        *fixed_owner,
        fixed_scene_owner,
        manual_schedule,
        *manual_owner,
        [&]() noexcept
        {
            return delayed_retry->state() ==
                lux::ecs::entity_scene::EEntitySectionState::ACTIVE;
        });
    // These tickets exist only to order the cancelled delayed load. Release
    // their live rows before the following dependency fixture asserts the
    // single common Section baseline.
    delayed_retry->reset();
    release_ticket->reset();
    manual_schedule.tick(0.0f, lux::ecs::kPhaseSceneLoading);

    // A request that can never fit the operation budget fails at acquire;
    // it must not enter an endless WAITING_ADMISSION retry loop.
    auto oversized = common.record;
    oversized.id = lux::ecs::scene_format::EntitySectionId{
        uuid("71000000-0000-4000-8000-000000000004")};
    oversized.encoded_bytes = 300u * 1024u * 1024u;
    auto rejected = manual_owner->client().acquire(std::move(oversized));
    assert(!rejected);
    assert(rejected.error() ==
        lux::ecs::entity_scene::EEntitySectionRequestError::INVALID_REQUEST);

    auto compressed = common.record;
    compressed.id = lux::ecs::scene_format::EntitySectionId{
        uuid("71000000-0000-4000-8000-000000000011")};
    compressed.compression =
        lux::ecs::scene_format::SectionCompression::ZSTD;
    compressed.encoded_bytes = 140u * 1024u * 1024u;
    compressed.decoded_bytes = 140u * 1024u * 1024u;
    auto compressed_rejected = manual_owner->client().acquire(
        std::move(compressed));
    assert(!compressed_rejected);
    assert(compressed_rejected.error() ==
        lux::ecs::entity_scene::EEntitySectionRequestError::INVALID_REQUEST);

    // NONE owns one moved byte image rather than simultaneous encoded and
    // decoded copies, so the same declaration is admissible below the queue
    // budget. The deliberately short provider payload then fails normally.
    auto uncompressed = common.record;
    uncompressed.id = lux::ecs::scene_format::EntitySectionId{
        uuid("71000000-0000-4000-8000-000000000012")};
    uncompressed.encoded_bytes = 200u * 1024u * 1024u;
    uncompressed.decoded_bytes = uncompressed.encoded_bytes;
    const auto stale_before_accounting =
        manual_owner->snapshot().stale_completions;
    auto uncompressed_admitted = manual_owner->client().acquire(
        std::move(uncompressed));
    assert(uncompressed_admitted);
    uncompressed_admitted->reset();
    drive(
        async,
        fixed_schedule,
        *fixed_owner,
        fixed_scene_owner,
        manual_schedule,
        *manual_owner,
        [&]() noexcept
        {
            return manual_owner->snapshot().stale_completions >
                stale_before_accounting;
        });

    auto unknown_compression = common.record;
    unknown_compression.id = lux::ecs::scene_format::EntitySectionId{
        uuid("71000000-0000-4000-8000-000000000013")};
    unknown_compression.compression = static_cast<
        lux::ecs::scene_format::SectionCompression>(0xffu);
    auto unknown_rejected = manual_owner->client().acquire(
        std::move(unknown_compression));
    assert(!unknown_rejected);
    assert(unknown_rejected.error() ==
        lux::ecs::entity_scene::EEntitySectionRequestError::INVALID_REQUEST);

    // A dependency must already have a ticket, then the dependent owns an
    // internal pin and cannot arm before that dependency is ACTIVE. Dropping
    // the caller's dependency ticket does not retire it under the dependent.
    auto dependent = common.record;
    dependent.id = lux::ecs::scene_format::EntitySectionId{
        uuid("71000000-0000-4000-8000-000000000014")};
    dependent.dependencies.push_back(common.record.id);
    auto dependent_ticket = manual_owner->client().acquire(dependent);
    assert(dependent_ticket);
    manual_ticket->reset();
    drive(
        async,
        fixed_schedule,
        *fixed_owner,
        fixed_scene_owner,
        manual_schedule,
        *manual_owner,
        [&]() noexcept
        {
            return dependent_ticket->state() ==
                lux::ecs::entity_scene::EEntitySectionState::FAILED;
        });
    assert(manual_world.registry().view<MarkerComponent>().size() == 2u);
    dependent_ticket->reset();
    manual_schedule.tick(0.0f, lux::ecs::kPhaseSceneLoading);
    assert(manual_world.registry().view<MarkerComponent>().empty());

    auto missing_dependency = dependent;
    missing_dependency.id = lux::ecs::scene_format::EntitySectionId{
        uuid("71000000-0000-4000-8000-000000000017")};
    missing_dependency.dependencies.front() =
        lux::ecs::scene_format::EntitySectionId{
            uuid("71000000-0000-4000-8000-000000000099")};
    const auto missing_dependency_result =
        manual_owner->client().acquire(std::move(missing_dependency));
    assert(!missing_dependency_result);
    assert(missing_dependency_result.error() ==
        lux::ecs::entity_scene::EEntitySectionRequestError::MISSING_DEPENDENCY);

    auto component_required = common.record;
    component_required.id = lux::ecs::scene_format::EntitySectionId{
        uuid("71000000-0000-4000-8000-000000000016")};
    component_required.required_components.push_back({
        lux::ecs::componentSchemaId("org.lux.test.marker"), 2u});
    auto unavailable_component = manual_owner->client().acquire(
        std::move(component_required));
    assert(!unavailable_component);
    assert(unavailable_component.error() ==
        lux::ecs::entity_scene::EEntitySectionRequestError::REQUIREMENT_UNAVAILABLE);

    // A generated Section does not require VFS and is reconstructed on the
    // background CPU arena. Its output still passes the exact same LXES,
    // digest, record and materialization checks as stored content.
    auto generated_valid = common.record;
    generated_valid.id = lux::ecs::scene_format::EntitySectionId{
        uuid("71000000-0000-4000-8000-000000000018")};
    generated_valid.source = lux::ecs::scene_format::GeneratedSectionSource{
        lux::ecs::scene_format::SectionGeneratorId{"org.lux.test.generator"},
        9u,
        {std::byte{0x2au}}};
    auto generated_image = makeMarkerImage(generated_valid.id);
    auto generated_bytes =
        lux::ecs::scene_format::encodeEntitySectionImage(generated_image);
    assert(generated_bytes);
    generated_valid.content_digest =
        lux::ecs::scene_format::entitySectionContentDigest(*generated_bytes);
    generated_valid.encoded_bytes = generated_bytes->size();
    generated_valid.decoded_bytes = generated_bytes->size();
    generated_valid.entity_count = generated_image.entities.size();
    auto generated_ticket =
        manual_owner->client().acquire(generated_valid);
    assert(generated_ticket);
    drive(
        async,
        fixed_schedule,
        *fixed_owner,
        fixed_scene_owner,
        manual_schedule,
        *manual_owner,
        [&]() noexcept
        {
            return generated_ticket->state() ==
                lux::ecs::entity_scene::EEntitySectionState::ACTIVE;
        });
    generated_ticket->reset();
    manual_schedule.tick(0.0f, lux::ecs::kPhaseSceneLoading);

    auto unknown_generator = generated_valid;
    unknown_generator.id = lux::ecs::scene_format::EntitySectionId{
        uuid("71000000-0000-4000-8000-000000000019")};
    std::get<lux::ecs::scene_format::GeneratedSectionSource>(
        unknown_generator.source).generator =
            lux::ecs::scene_format::SectionGeneratorId{
                "org.lux.test.unknown_generator"};
    auto unavailable_source =
        manual_owner->client().acquire(std::move(unknown_generator));
    assert(!unavailable_source);
    assert(unavailable_source.error() ==
        lux::ecs::entity_scene::EEntitySectionRequestError::SOURCE_UNAVAILABLE);

    // A registered generator with output that disagrees with the declared
    // digest fails asynchronously. The failure is adopted only after the
    // MainThreadScheduler hop, and its terminal generation remains stable
    // while tickets still refer to it.
    auto generated_mismatch = common.record;
    generated_mismatch.id = lux::ecs::scene_format::EntitySectionId{
        uuid("71000000-0000-4000-8000-000000000005")};
    generated_mismatch.source = lux::ecs::scene_format::GeneratedSectionSource{
        lux::ecs::scene_format::SectionGeneratorId{"org.lux.test.generator"},
        7u,
        {}};
    auto mismatch_ticket =
        manual_owner->client().acquire(generated_mismatch);
    assert(mismatch_ticket);
    drive(
        async,
        fixed_schedule,
        *fixed_owner,
        fixed_scene_owner,
        manual_schedule,
        *manual_owner,
        [&]() noexcept
        {
            return mismatch_ticket->state() ==
                lux::ecs::entity_scene::EEntitySectionState::FAILED;
        });
    // A terminal generation remains immutable while any tickets refer to it.
    // Reacquiring the same record shares that FAILED generation rather than
    // replacing the slot under the first ticket.
    auto same_failed = manual_owner->client().acquire(generated_mismatch);
    assert(same_failed);
    assert(same_failed->generation() == mismatch_ticket->generation());
    const auto failed_generation = same_failed->generation();
    mismatch_ticket->reset();
    assert(same_failed->state() == lux::ecs::entity_scene::EEntitySectionState::FAILED);
    same_failed->reset();
    auto mismatch_retry =
        manual_owner->client().acquire(generated_mismatch);
    assert(mismatch_retry);
    assert(mismatch_retry->generation() > failed_generation);
    drive(
        async,
        fixed_schedule,
        *fixed_owner,
        fixed_scene_owner,
        manual_schedule,
        *manual_owner,
        [&]() noexcept
        {
            return mismatch_retry->state() ==
                lux::ecs::entity_scene::EEntitySectionState::FAILED;
        });
    mismatch_retry->reset();

    // Historical Section identities do not grow owner traversal forever.
    // A cancelled slot is removed from the id map and its index is reused;
    // the monotonically advancing generation rejects late completions.
    const auto before_history = manual_owner->snapshot();
    for (std::uint64_t ordinal = 0u; ordinal < 256u; ++ordinal)
    {
        auto transient = common.record;
        transient.id = lux::ecs::scene_format::EntitySectionId{
            ordinalUuid(1000u + ordinal)};
        auto ticket = manual_owner->client().acquire(std::move(transient));
        assert(ticket);
        ticket->reset();
    }
    const auto after_history = manual_owner->snapshot();
    assert(after_history.allocated_slots <=
        before_history.allocated_slots + 1u);
    assert(after_history.section_mappings ==
        before_history.section_mappings);

    auto fixed_client_before_close = fixed_owner->client();
    closeScene(*fixed_scene_owner, async, fixed_schedule);
    // Startup selection owns only its tickets. Closing it must not close the
    // shared loader used by manual/spatial selectors in the same registry.
    assert(fixed_owner->client());
    auto after_scene_close =
        fixed_client_before_close.acquire(common.record);
    assert(after_scene_close);
    after_scene_close->reset();
    closeOwner(*fixed_owner, async, fixed_schedule);
    closeOwner(*manual_owner, async, manual_schedule);
    assert(fixed_owner->snapshot().active_sections == 0u);
    assert(manual_owner->snapshot().active_sections == 0u);
    assert(fixed_owner->snapshot().armed_sections == 0u);
    assert(manual_owner->snapshot().armed_sections == 0u);
    assert(fixed_owner->snapshot().waiting_sections == 0u);
    assert(manual_owner->snapshot().waiting_sections == 0u);
    assert(fixed_owner->snapshot().staging_sections == 0u);
    assert(manual_owner->snapshot().staging_sections == 0u);
    assert(fixed_owner->snapshot().outstanding_tickets == 0u);
    assert(manual_owner->snapshot().outstanding_tickets == 0u);
    assert(fixed_owner->snapshot().section_mappings == 0u);
    assert(manual_owner->snapshot().section_mappings == 0u);
    assert(fixed_owner->snapshot().free_slots ==
        fixed_owner->snapshot().allocated_slots);
    assert(manual_owner->snapshot().free_slots ==
        manual_owner->snapshot().allocated_slots);
    assert(fixed_owner->snapshot().blobs.current_bytes == 0u);
    assert(manual_owner->snapshot().blobs.current_bytes == 0u);

    // A syntactically valid but unavailable component requirement fails on
    // the first loading tick, before any startup ticket or live entity exists.
    // The failed selector releases only its own interests; it does not close
    // the shared loader owned by composition root.
    lux::ecs::World rejected_world;
    lux::ecs::PersistentEntityIndex rejected_persistent_entities{
        rejected_world.registry()};
    lux::ecs::Schedule rejected_schedule{rejected_world};
    auto rejected_loader =
        std::make_unique<lux::ecs::entity_scene::EntitySectionLoaderSystem>(
            service.loadClient(),
            vfs,
            std::make_unique<runtime::SectionBlobStore>(),
            components,
            rejected_persistent_entities);
    auto* rejected_loader_owner = rejected_loader.get();
    assert(rejected_schedule.addSystem(
        std::move(rejected_loader), lux::ecs::kPhaseSceneLoading));
    lux::scene::SceneDescription rejected_package;
    rejected_package.id = lux::asset::asset_id_t{
        uuid("70000000-0000-4000-8000-000000000002")};
    rejected_package.startup_sections.push_back(common.record.id);
    rejected_package.sections.push_back(common.record);
    rejected_package.required_components.push_back({
        lux::ecs::componentSchemaId("org.lux.test.unknown"), 1u});
    auto rejected_catalog_result = runtime::EntitySceneCatalog::create(
        std::move(rejected_package));
    assert(rejected_catalog_result);
    auto rejected_catalog = std::move(*rejected_catalog_result);
    const auto& rejected_scene_package = rejected_catalog.package();
    auto rejected_scene_result =
        lux::ecs::entity_scene::StartupSectionSystem::create(
            rejected_scene_package.id,
            rejected_scene_package.sections,
            rejected_scene_package.startup_sections,
            rejected_scene_package.required_components,
            *rejected_loader_owner);
    assert(rejected_scene_result);
    auto rejected_scene = std::move(*rejected_scene_result);
    auto* rejected_scene_owner = rejected_scene.get();
    assert(rejected_schedule.addSystem(
        std::move(rejected_scene), lux::ecs::kPhaseSceneLoading));
    assert(rejected_schedule.compile().valid());
    rejected_schedule.tick(0.0f, lux::ecs::kPhaseSceneLoading);
    assert(rejected_scene_owner->state() ==
        lux::ecs::entity_scene::EEntitySceneState::FAILED);
    assert(rejected_scene_owner->revision() == 0u);
    assert(rejected_scene_owner->failure());
    assert(rejected_scene_owner->failure()->error ==
        lux::ecs::entity_scene::EEntitySceneError::REQUIREMENT_UNAVAILABLE);
    assert(rejected_world.registry().view<MarkerComponent>().empty());
    assert(rejected_loader_owner->snapshot().section_mappings == 0u);
    rejected_scene_owner->requestClose();
    rejected_schedule.tick(0.0f, lux::ecs::kPhaseSceneLoading);
    assert(rejected_scene_owner->closeComplete());
    assert(rejected_scene_owner->state() ==
        lux::ecs::entity_scene::EEntitySceneState::CLOSED);
    assert(rejected_loader_owner->snapshot().blobs.current_bytes == 0u);
    assert(rejected_loader_owner->client());
    closeOwner(*rejected_loader_owner, async, rejected_schedule);

    // Section generations are process-wide, not an address-local counter in
    // one loader. Reconstructing a loader cannot make an old ticket/completion
    // generation numerically valid again.
    lux::ecs::World rebuilt_world;
    lux::ecs::PersistentEntityIndex rebuilt_persistent_entities{
        rebuilt_world.registry()};
    lux::ecs::Schedule rebuilt_schedule{rebuilt_world};
    auto rebuilt_loader =
        std::make_unique<lux::ecs::entity_scene::EntitySectionLoaderSystem>(
            service.loadClient(),
            vfs,
            std::make_unique<runtime::SectionBlobStore>(),
            components,
            rebuilt_persistent_entities);
    auto* rebuilt_loader_owner = rebuilt_loader.get();
    assert(rebuilt_schedule.addSystem(
        std::move(rebuilt_loader), lux::ecs::kPhaseSceneLoading));
    assert(rebuilt_schedule.compile().valid());
    auto rebuilt_ticket =
        rebuilt_loader_owner->client().acquire(common.record);
    assert(rebuilt_ticket);
    assert(rebuilt_ticket->generation() > first_loader_generation);
    rebuilt_ticket->reset();
    closeOwner(*rebuilt_loader_owner, async, rebuilt_schedule);
    assert(rebuilt_loader_owner->snapshot().section_mappings == 0u);
    assert(rebuilt_loader_owner->snapshot().blobs.current_bytes == 0u);

    service.close();
    const auto close = lux::exec::testing::closeRuntime(async);
    assert(close.clean());
    return 0;
}
