#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/ecs/PersistentEntityIndex.hpp>
#include <lux/engine/ecs/scene_format/EntitySectionCodec.hpp>
#include <lux/engine/ecs/components/Transform3DComponent.hpp>
#include <lux/engine/ecs/spatial3d/components/SpatialInterest3DComponent.hpp>
#include <lux/engine/ecs/transform/systems/Transform3DSystem.hpp>
#include <lux/engine/input/ActionMapper.hpp>
#include <lux/engine/resource/asset/AssetCodecCatalog.hpp>
#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/scene/ScenePackageCodec.hpp>
#include <lux/engine/ecs/scene_format/EntitySectionCodec.hpp>
#include <lux/engine/spatial3d/SceneCatalog.hpp>
#include <lux/engine/runtime/assets/AssetLoadService.hpp>
#include <lux/engine/runtime/entity_scene/EntitySectionService.hpp>
#include <lux/engine/runtime/execution/AsyncRuntime.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeBuilder.hpp>
#include <lux/engine/runtime/execution/testing/AsyncCloseTestDriver.hpp>
#include <lux/engine/runtime/scene/SceneRuntime.hpp>
#include <lux/engine/runtime/spatial3d/partitioned/Spatial3DPartitionedContribution.hpp>
#include <lux/engine/runtime/spatial3d/partitioned/SpatialInterest3DSystem.hpp>
#include <lux/engine/runtime/spatial3d/transform/Spatial3DTransformContribution.hpp>
#include <lux/engine/runtime/spatial_partition/SpatialPartitionSystem.hpp>

#include <algorithm>
#include <cassert>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    [[nodiscard]] uuids::uuid uuid(const char* value)
    {
        return uuids::uuid::from_string(value).value();
    }

    [[nodiscard]] bool uuidLess(
        const uuids::uuid& left,
        const uuids::uuid& right) noexcept
    {
        return std::ranges::lexicographical_compare(
            left.as_bytes(), right.as_bytes());
    }

    struct StoredImage final
    {
        lux::asset::asset_id_t asset;
        std::string path;
        lux::asset::AssetBlob image;
    };

    class SectionProvider final : public lux::asset::IAssetProvider
    {
    public:
        explicit SectionProvider(std::vector<StoredImage> images) noexcept
            : images_(std::move(images))
        {}

        [[nodiscard]] std::optional<lux::asset::asset_id_t> resolve(
            std::string_view path) const override
        {
            const auto found = std::ranges::find(
                images_, path, &StoredImage::path);
            return found == images_.end()
                ? std::nullopt
                : std::optional{found->asset};
        }

        [[nodiscard]] bool contains(
            const lux::asset::asset_id_t& id) const override
        {
            return std::ranges::find(images_, id, &StoredImage::asset) !=
                images_.end();
        }

        [[nodiscard]] lux::cxx::expected<
            lux::asset::AssetBlob,
            lux::asset::EAssetError>
        open(const lux::asset::asset_id_t& id) const override
        {
            const auto found = std::ranges::find(
                images_, id, &StoredImage::asset);
            if (found == images_.end())
            {
                return lux::cxx::unexpected(
                    lux::asset::EAssetError::ASSET_NOT_EXIST);
            }
            return found->image;
        }

        void enumerate(
            const std::function<void(const lux::asset::ProviderEntry&)>& fn)
            const override
        {
            for (const auto& image : images_)
            {
                fn({
                    image.asset,
                    lux::asset::EAssetType::UNKNOWN,
                    image.path,
                    false});
            }
        }

        [[nodiscard]] std::optional<std::string> pathOf(
            const lux::asset::asset_id_t& id) const override
        {
            const auto found = std::ranges::find(
                images_, id, &StoredImage::asset);
            return found == images_.end()
                ? std::nullopt
                : std::optional{found->path};
        }

    private:
        std::vector<StoredImage> images_;
    };

    struct SectionFixture final
    {
        lux::scene::SectionRecord record;
        lux::ecs::PersistentEntityId entity;
        StoredImage stored;
    };

    [[nodiscard]] SectionFixture makeSection(
        const char* section_id,
        const char* entity_id,
        const char* asset_id,
        std::string path,
        std::string_view channel)
    {
        namespace format = lux::ecs::scene_format;
        namespace scene = lux::scene;

        format::EntitySectionImage image;
        image.section = format::EntitySectionId{uuid(section_id)};
        image.component_names = {""};
        image.archetypes.push_back({});
        const lux::ecs::PersistentEntityId persistent{uuid(entity_id)};
        image.entities.push_back({0u, persistent});
        auto encoded = format::encodeEntitySectionImage(image);
        assert(encoded);

        scene::SectionRecord record;
        record.id = image.section;
        record.source = scene::StoredSectionSource{"/Game/" + path};
        record.content_digest = format::entitySectionContentDigest(*encoded);
        record.encoded_bytes = encoded->size();
        record.decoded_bytes = encoded->size();
        record.entity_count = 1u;
        record.demand_channels.emplace_back(std::string{channel});

        auto blob = lux::asset::AssetBlob::fromShared(
            lux::cxx::SharedBytes<>::copyOf(
                std::span<const std::byte>{
                    encoded->data(), encoded->size()}));
        return {
            std::move(record),
            persistent,
            StoredImage{uuid(asset_id), std::move(path), std::move(blob)}};
    }

    template<class Predicate>
    void driveScene(
        lux::runtime::SceneRuntime& scene,
        lux::exec::AsyncRuntime& runtime,
        Predicate&& done)
    {
        lux::input::ActionMapper input;
        lux::exec::testing::CloseEpoch progress{runtime};
        progress.driveWithStep(
            [&]() noexcept { scene.tick(0.0f, 0.0f, 0.0f, input); },
            std::forward<Predicate>(done),
            [&]() noexcept
            {
                const auto state = scene.entitySectionLoaderSnapshot();
                return state.waiting_admission_sections != 0u ||
                    state.waiting_sections != 0u ||
                    state.staging_sections != 0u ||
                    state.armed_sections != 0u;
            });
        assert(done());
    }

    [[nodiscard]] lux::runtime::SceneCloseReport closeScene(
        lux::runtime::SceneRuntime& scene,
        lux::exec::AsyncRuntime& runtime)
    {
        lux::exec::testing::CloseEpoch progress{runtime};
        std::optional<lux::runtime::SceneCloseReport> report;
        lux::runtime::detail::subscribeSceneClose(
            scene,
            [&progress, &report](auto value) noexcept
            {
                report = value;
                progress.notify();
            });
        progress.drive([&report]() noexcept { return report.has_value(); });
        return *report;
    }
}

int main()
{
    namespace partition = lux::runtime::spatial_partition;
    namespace spatial3d = lux::runtime::spatial3d;
    namespace catalog = lux::spatial3d;

    lux::meta::meta_module_init();
    lux::ecs::ComponentTypeCatalog components;
    assert(lux::ecs::registerGeneratedComponents(components));

    constexpr auto channel =
        catalog::kVisualLodDemandChannelName;
    auto fine_origin = makeSection(
        "51000000-0000-4000-8000-000000000001",
        "52000000-0000-4000-8000-000000000001",
        "53000000-0000-4000-8000-000000000001",
        "fine_origin_lxes",
        channel);
    auto fine_next = makeSection(
        "51000000-0000-4000-8000-000000000002",
        "52000000-0000-4000-8000-000000000002",
        "53000000-0000-4000-8000-000000000002",
        "fine_next_lxes",
        channel);
    auto coarse_origin = makeSection(
        "51000000-0000-4000-8000-000000000003",
        "52000000-0000-4000-8000-000000000003",
        "53000000-0000-4000-8000-000000000003",
        "coarse_origin_lxes",
        channel);
    auto coarse_next = makeSection(
        "51000000-0000-4000-8000-000000000004",
        "52000000-0000-4000-8000-000000000004",
        "53000000-0000-4000-8000-000000000004",
        "coarse_next_lxes",
        channel);

    catalog::SceneCatalogBand fine_band{
        catalog::SourceId{"lux.spatial3d.source.test"},
        lux::scene::DemandChannelId{std::string{channel}},
        0u,
        64.0,
        1.0,
        1.0};
    catalog::SceneCatalogBand coarse_band{
        catalog::SourceId{"lux.spatial3d.source.test"},
        lux::scene::DemandChannelId{std::string{channel}},
        1u,
        128.0,
        1.0,
        1.0};
    const auto fine_namespace =
        lux::runtime::spatial3DDemandSourceNamespace(fine_band);
    const auto coarse_namespace =
        lux::runtime::spatial3DDemandSourceNamespace(coarse_band);
    assert(fine_namespace != coarse_namespace);
    const catalog::SceneCatalogBand unrelated_band{
        catalog::SourceId{"lux.spatial3d.source.aaa"},
        lux::scene::DemandChannelId{
            std::string{catalog::kResidentDemandChannelName}},
        0u,
        32.0,
        1.0,
        1.0};
    assert(lux::runtime::spatial3DDemandSourceNamespace(fine_band) ==
        fine_namespace);
    assert(lux::runtime::spatial3DDemandSourceNamespace(unrelated_band) !=
        fine_namespace);
    std::vector<catalog::SceneCatalogBand>
        bands_with_unrelated{fine_band, coarse_band};
    bands_with_unrelated.insert(
        bands_with_unrelated.begin(), unrelated_band);
    const auto relocated_fine = std::ranges::find_if(
        bands_with_unrelated,
        [channel](const auto& band)
        {
            return band.source.name() == "lux.spatial3d.source.test" &&
                band.demand_channel.name() == channel &&
                band.level == 0u;
        });
    assert(relocated_fine != bands_with_unrelated.end());
    assert(lux::runtime::spatial3DDemandSourceNamespace(*relocated_fine) ==
        fine_namespace);
    const catalog::SceneCatalogBand ambiguous_left{
        catalog::SourceId{"a.b"},
        lux::scene::DemandChannelId{"c.d.e"},
        0u,
        64.0,
        1.0,
        1.0};
    const catalog::SceneCatalogBand ambiguous_right{
        catalog::SourceId{"a.b.c"},
        lux::scene::DemandChannelId{"d.e"},
        0u,
        64.0,
        1.0,
        1.0};
    assert(lux::runtime::spatial3DDemandSourceNamespace(ambiguous_left) !=
        lux::runtime::spatial3DDemandSourceNamespace(ambiguous_right));

    catalog::SceneCatalog spatial_config;
    spatial_config.bands = {fine_band, coarse_band};
    spatial_config.entries = {
        {{0, 0, 0}, 0u, fine_origin.record.id},
        {{1, 0, 0}, 0u, fine_next.record.id},
        {{0, 0, 0}, 1u, coarse_origin.record.id},
        {{1, 0, 0}, 1u, coarse_next.record.id}};
    auto encoded_config =
        catalog::encodeSceneCatalog(spatial_config);
    assert(encoded_config);

    lux::scene::ScenePackage package;
    package.id = lux::scene::ScenePackageId{
        uuid("50000000-0000-4000-8000-000000000001")};
    package.features.push_back({
        lux::scene::SceneFeatureId{
            std::string{catalog::kPartitionedFeatureName}},
        catalog::kSceneCatalogSchemaVersion,
        std::move(*encoded_config)});
    package.sections = {
        fine_origin.record,
        fine_next.record,
        coarse_origin.record,
        coarse_next.record};
    std::ranges::sort(
        package.sections,
        [](const auto& left, const auto& right)
        {
            return uuidLess(left.id.value(), right.id.value());
        });
    auto package_bytes = lux::scene::encodeScenePackage(package);
    assert(package_bytes);

    std::vector<StoredImage> stored;
    stored.push_back(std::move(fine_origin.stored));
    stored.push_back(std::move(fine_next.stored));
    stored.push_back(std::move(coarse_origin.stored));
    stored.push_back(std::move(coarse_next.stored));
    auto vfs = std::make_shared<lux::asset::AssetVfs>();
    assert(vfs->mount({
        "/Game",
        std::make_shared<SectionProvider>(std::move(stored)),
        0}) != lux::asset::kInvalidMountId);

    lux::asset::AssetManager assets{
        lux::asset::runtimeAssetCodecCatalog()};
    lux::exec::AsyncRuntimeBuilder async_builder;
    auto assets_service =
        lux::asset_runtime::AssetLoadService::addTo(async_builder, assets);
    auto sections_service =
        lux::runtime::entity_scene::EntitySectionService::addTo(async_builder);
    assert(assets_service && sections_service);
    auto async_plan = std::move(async_builder).compile();
    assert(async_plan);
    lux::exec::AsyncRuntime async{std::move(*async_plan)};
    auto asset_owner = std::move(*assets_service);
    auto section_owner = std::move(*sections_service);

    lux::runtime::SceneContributionCatalog contributions;
    auto transform_descriptor =
        lux::runtime::makeSpatial3DTransformContribution(components);
    auto descriptor =
        lux::runtime::makeSpatial3DPartitionedContribution(components);
    assert(transform_descriptor && descriptor);
    assert(contributions.add(std::move(*transform_descriptor)));
    assert(contributions.add(std::move(*descriptor)));
    lux::runtime::SceneRuntime::Dependencies dependencies{
        .assets = assets,
        .asset_client = asset_owner.client(),
        .async = async,
        .components = components,
        .entity_sections = section_owner.loadClient(),
        .scene_contribution_catalog = &contributions};
    lux::runtime::SceneRuntime::Config scene_config;
    scene_config.name = "Spatial3D LXSC integration";
    scene_config.scene_origin = "/Game/spatial3d_scene_lxsc";
    scene_config.scene_package_image = lux::asset::AssetBlob::fromShared(
        lux::cxx::SharedBytes<>::copyOf(
            std::span<const std::byte>{
                package_bytes->data(), package_bytes->size()}));
    scene_config.section_vfs = vfs;
    auto scene = lux::runtime::SceneRuntime::create(
        dependencies, scene_config);
    assert(scene);

    lux::input::ActionMapper input;
    scene->tick(0.0f, 0.0f, 0.0f, input);
    assert(scene->isReady());
    auto* const partition_owner = scene->services().get<
        partition::SpatialPartitionSystem>();
    auto* const interest_owner = scene->services().get<
        spatial3d::SpatialInterest3DSystem>();
    auto* const persistent = scene->services().get<
        lux::ecs::PersistentEntityIndex>();
    auto* const transform = scene->services().get<
        lux::ecs::Transform3DSystem>();
    assert(partition_owner && interest_owner && persistent && transform);
    assert(partition_owner->snapshot().demand.maximum_decoded_bytes ==
        spatial_config.residency.maximum_decoded_bytes);
    assert(partition_owner->snapshot().demand.maximum_entities ==
        spatial_config.residency.maximum_entities);
    assert(interest_owner->snapshot().maximum_sources ==
        spatial_config.residency.maximum_interest_sources);
    assert(interest_owner->snapshot().maximum_sections_per_source ==
        spatial_config.residency.maximum_sections_per_interest);

    auto& registry = scene->world().registry();
    const auto interest_entity = registry.create();
    registry.emplace<lux::ecs::SpatialInterest3DComponent>(
        interest_entity,
        lux::ecs::SpatialInterest3DComponent{
            .active_distance = 0.0,
            .resident_distance = 0.0});
    registry.emplace<lux::ecs::Transform3DComponent>(
        interest_entity);
    driveScene(*scene, async, [&]() noexcept
    {
        return partition_owner->snapshot().active_sections == 2u &&
            scene->entitySectionLoaderSnapshot().active_sections == 2u &&
            persistent->find(fine_origin.entity) != entt::null &&
            persistent->find(coarse_origin.entity) != entt::null;
    });
    assert(persistent->find(fine_next.entity) == entt::null);
    assert(persistent->find(coarse_next.entity) == entt::null);

    registry.patch<lux::ecs::Transform3DComponent>(
        interest_entity,
        [](auto& transform) noexcept
        {
            transform.position.x = 64.0;
        });
    driveScene(*scene, async, [&]() noexcept
    {
        return partition_owner->snapshot().active_sections == 2u &&
            scene->entitySectionLoaderSnapshot().active_sections == 2u &&
            persistent->find(fine_origin.entity) == entt::null &&
            persistent->find(fine_next.entity) != entt::null &&
            persistent->find(coarse_origin.entity) != entt::null;
    });

    registry.destroy(interest_entity);
    driveScene(*scene, async, [&]() noexcept
    {
        const auto selected = interest_owner->snapshot();
        const auto partitioned = partition_owner->snapshot();
        const auto loaded = scene->entitySectionLoaderSnapshot();
        return selected.tracked_sources == 0u &&
            partitioned.demand.source_count == 0u &&
            partitioned.loader_tickets == 0u &&
            loaded.active_sections == 0u &&
            loaded.outstanding_tickets == 0u;
    });
    assert(closeScene(*scene, async).clean());
    scene.reset();

    // The opaque catalog is an exact index over LXSC, not an optional hint.
    // A spatial-channel record with no source/band/cell entry rejects the
    // whole scene before any entity reaches the live registry.
    auto unmatched_package = package;
    auto unmatched_record = unmatched_package.sections.front();
    unmatched_record.id = lux::ecs::scene_format::EntitySectionId{
        uuid("51000000-0000-4000-8000-000000000005")};
    unmatched_package.sections.push_back(std::move(unmatched_record));
    std::ranges::sort(
        unmatched_package.sections,
        [](const auto& left, const auto& right)
        {
            return uuidLess(left.id.value(), right.id.value());
        });
    auto unmatched_bytes = lux::scene::encodeScenePackage(
        unmatched_package);
    assert(unmatched_bytes);
    auto rejected_config = scene_config;
    rejected_config.scene_package_image = lux::asset::AssetBlob::fromShared(
        lux::cxx::SharedBytes<>::copyOf(
            std::span<const std::byte>{
                unmatched_bytes->data(), unmatched_bytes->size()}));
    auto rejected_scene = lux::runtime::SceneRuntime::create(
        dependencies, rejected_config);
    assert(!rejected_scene);

    section_owner.close();
    asset_owner.close();
    assert(lux::exec::testing::closeRuntime(async).clean());
    return 0;
}
