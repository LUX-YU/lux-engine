#include <lux/engine/runtime/scene/SceneRuntime.hpp>

#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/ecs/PersistentEntityIndex.hpp>
#include <lux/engine/ecs/scene_format/EntitySectionCodec.hpp>
#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/components/ParentComponent.hpp>
#include <lux/engine/resource/asset/AssetCodecCatalog.hpp>
#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/scene/SceneAssetSerDeser.hpp>
#include <lux/engine/runtime/assets/AssetLoadService.hpp>
#include <lux/engine/runtime/entity_scene/EntitySectionService.hpp>
#include <lux/engine/runtime/entity_scene/SectionBlobStore.hpp>
#include <lux/engine/runtime/execution/AsyncRuntime.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeBuilder.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeSenders.hpp>
#include <lux/engine/runtime/execution/AsyncScope.hpp>
#include <lux/engine/runtime/execution/AsyncScopeSenders.hpp>
#include <lux/engine/runtime/execution/testing/AsyncCloseTestDriver.hpp>
#include <lux/engine/input/ActionMapper.hpp>

#include <stdexec/execution.hpp>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
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
    int failures = 0;

    void check(bool condition, const char* label)
    {
        std::printf("  [%s] %s\n", condition ? "OK" : "FAIL", label);
        if (!condition)
            ++failures;
    }

    [[nodiscard]] uuids::uuid uuid(const char* value)
    {
        return uuids::uuid::from_string(value).value();
    }

    [[nodiscard]] lux::asset::asset_id_t registerScene(
        lux::asset::AssetManager& assets,
        lux::scene::SceneDescription description)
    {
        const auto id = description.id;
        auto info = std::make_unique<lux::asset::AssetInfo>();
        info->id = id;
        info->type = lux::scene::kSceneAssetType;
        if (!assets.registerAsset(std::make_unique<lux::scene::SceneAsset>(
                std::move(info),
                std::make_unique<lux::scene::SceneDescription>(
                    std::move(description)))))
        {
            return {};
        }
        return id;
    }

    class SectionProvider final : public lux::asset::IAssetProvider
    {
    public:
        SectionProvider(
            lux::asset::asset_id_t id,
            std::string path,
            lux::asset::AssetBlob image) noexcept
            : id_(id), path_(std::move(path)), image_(std::move(image))
        {}

        [[nodiscard]] std::optional<lux::asset::asset_id_t> resolve(
            std::string_view path) const override
        {
            return path == path_ ? std::optional{id_} : std::nullopt;
        }

        [[nodiscard]] bool contains(
            const lux::asset::asset_id_t& id) const override
        {
            return id == id_;
        }

        [[nodiscard]] lux::cxx::expected<
            lux::asset::AssetBlob,
            lux::asset::EAssetError>
        open(const lux::asset::asset_id_t& id) const override
        {
            if (id != id_)
            {
                return lux::cxx::unexpected(
                    lux::asset::EAssetError::ASSET_NOT_EXIST);
            }
            return image_;
        }

        void enumerate(
            const std::function<void(const lux::asset::ProviderEntry&)>& fn)
            const override
        {
            fn({id_, lux::ecs::scene_format::kEntitySectionImageMagic,
                path_, false});
        }

        [[nodiscard]] std::optional<std::string> pathOf(
            const lux::asset::asset_id_t& id) const override
        {
            return id == id_ ? std::optional{path_} : std::nullopt;
        }

    private:
        lux::asset::asset_id_t id_{};
        std::string path_;
        lux::asset::AssetBlob image_;
    };

    [[nodiscard]] lux::runtime::SceneCloseReport closeScene(
        lux::runtime::SceneRuntime& scene,
        lux::exec::AsyncRuntime& runtime,
        std::size_t& completion_count)
    {
        lux::exec::testing::CloseEpoch progress{runtime};
        std::optional<lux::runtime::SceneCloseReport> report;
        lux::runtime::detail::subscribeSceneClose(
            scene,
            [&progress, &report, &completion_count](
                lux::runtime::SceneCloseReport value) noexcept
            {
                report = value;
                ++completion_count;
                progress.notify();
            });
        progress.drive(
            [&report]() noexcept { return report.has_value(); });
        return *report;
    }

    struct RuntimeCloseProbe final
    {
        std::vector<int> trace;
        bool child_admitted{false};
        bool child_terminal{false};
        bool scope_terminal{false};
        bool contribution_service_alive{false};
        bool system_destroyed_after_service{false};
    };

    class RuntimeCloseService final
    {
    public:
        explicit RuntimeCloseService(RuntimeCloseProbe& probe) noexcept
            : probe_(&probe)
        {
            probe_->contribution_service_alive = true;
        }

        ~RuntimeCloseService() noexcept
        {
            probe_->contribution_service_alive = false;
        }

    private:
        RuntimeCloseProbe* probe_{};
    };

    class RuntimeCloseProviderSystem final : public lux::ecs::ISystem
    {
    public:
        explicit RuntimeCloseProviderSystem(RuntimeCloseProbe& probe) noexcept
            : probe_(&probe)
        {}

        ~RuntimeCloseProviderSystem() override
        {
            if (!probe_->contribution_service_alive)
                probe_->system_destroyed_after_service = true;
            if (!removed_)
                probe_->trace.push_back(-10);
        }

        void update(const lux::ecs::SystemUpdateContext&) override {}

        void requestClose() noexcept override
        {
            if (closing_)
                return;
            closing_ = true;
            probe_->trace.push_back(10);
        }

        [[nodiscard]] bool closeComplete() const noexcept override
        {
            return closing_;
        }

        [[nodiscard]] bool supportsDynamicRemoval() const noexcept override
        {
            return true;
        }

        void onRemoved(const lux::ecs::SystemRemovalContext&) override
        {
            if (!probe_->contribution_service_alive)
                probe_->system_destroyed_after_service = true;
            removed_ = true;
            probe_->trace.push_back(-10);
        }

    private:
        RuntimeCloseProbe* probe_{};
        bool closing_{false};
        bool removed_{false};
    };

    class RuntimeCloseAsyncConsumerSystem final : public lux::ecs::ISystem
    {
    public:
        RuntimeCloseAsyncConsumerSystem(
            lux::exec::AsyncRuntime& runtime,
            RuntimeCloseProbe& probe)
            : scope_(runtime), probe_(&probe)
        {
            auto child = stdexec::schedule(
                    lux::exec::mainThreadScheduler(runtime))
                | stdexec::then(
                      [this]() noexcept
                      {
                          probe_->child_terminal = true;
                      })
                | stdexec::upon_stopped(
                      [this]() noexcept
                      {
                          probe_->child_terminal = true;
                      });
            probe_->child_admitted = lux::exec::spawn(
                scope_, std::move(child));
        }

        ~RuntimeCloseAsyncConsumerSystem() override
        {
            if (!probe_->contribution_service_alive)
                probe_->system_destroyed_after_service = true;
            if (!removed_)
                probe_->trace.push_back(-20);
        }

        [[nodiscard]] std::span<const lux::ecs::SystemType>
        prerequisites() const noexcept override
        {
            static constexpr lux::ecs::SystemType required[]{
                lux::ecs::systemType<RuntimeCloseProviderSystem>()};
            return required;
        }

        void update(const lux::ecs::SystemUpdateContext&) override {}

        void requestClose() noexcept override { requestClose({}); }

        void requestClose(lux::ecs::SystemCloseProgressSink progress)
            noexcept override
        {
            // SceneContributionHost closes admission before SceneRuntime
            // supplies its wake sink through the whole Schedule. A repeated
            // request must therefore upgrade the retained sink without
            // starting a second close subscription.
            if (progress)
                progress_ = progress;
            if (closing_)
                return;
            closing_ = true;
            probe_->trace.push_back(20);
            lux::exec::detail::subscribeScopeClose(
                scope_,
                [this]() noexcept
                {
                    probe_->scope_terminal = true;
                    closed_ = true;
                    progress_.notify();
                });
        }

        [[nodiscard]] bool closeComplete() const noexcept override
        {
            return closing_ && closed_;
        }

        [[nodiscard]] bool closeNeedsOwnerTick() const noexcept override
        {
            return false;
        }

        void onRemoved(const lux::ecs::SystemRemovalContext&) override
        {
            if (!probe_->contribution_service_alive)
                probe_->system_destroyed_after_service = true;
            removed_ = true;
            probe_->trace.push_back(-20);
        }

    private:
        lux::exec::AsyncScope scope_;
        RuntimeCloseProbe* probe_{};
        lux::ecs::SystemCloseProgressSink progress_;
        bool closing_{false};
        bool closed_{false};
        bool removed_{false};
    };

    [[nodiscard]] lux::runtime::SceneContributionDescriptor
    makeRuntimeCloseContribution(
        lux::exec::AsyncRuntime& runtime,
        RuntimeCloseProbe& probe)
    {
        lux::runtime::SceneContributionDescriptor descriptor;
        descriptor.id = lux::scene::SceneFeatureId{
            "org.test.scene.runtime_close"};
        descriptor.display_name = "SceneRuntime close ownership probe";
        descriptor.provided_services = {
            lux::cxx::typeToken<RuntimeCloseService>()};
        descriptor.config_schema_version = 0u;
        descriptor.provider = lux::extensions::ExtensionId{
            "org.test.runtime_close"};
        descriptor.build = [&runtime, &probe](
            lux::runtime::SceneContributionBatchBuilder& builder,
            const lux::runtime::SceneContributionBuildContext&,
            lux::runtime::ContributionConfig)
            -> lux::cxx::expected<
                void,
                lux::runtime::SceneContributionBuildFailure>
        {
            const auto service = builder.publishService(
                std::make_unique<RuntimeCloseService>(probe));
            if (!service)
                return lux::cxx::unexpected(service.error());
            const auto provider = builder.add(
                std::make_unique<RuntimeCloseProviderSystem>(probe));
            if (!provider)
                return lux::cxx::unexpected(provider.error());
            return builder.add(
                std::make_unique<RuntimeCloseAsyncConsumerSystem>(
                    runtime, probe));
        };
        return descriptor;
    }

    struct CrossPhaseBlobCloseProbe final
    {
        lux::runtime::entity_scene::ContentBlobLease lease;
        bool close_requested{false};
        bool released_on_simulation_tick{false};
    };

    class CrossPhaseBlobCloseSystem final : public lux::ecs::ISystem
    {
    public:
        explicit CrossPhaseBlobCloseSystem(
            CrossPhaseBlobCloseProbe& probe) noexcept
            : probe_(&probe)
        {}

        void update(const lux::ecs::SystemUpdateContext&) override
        {
            if (!probe_->close_requested || !probe_->lease)
                return;
            probe_->lease = {};
            probe_->released_on_simulation_tick = true;
        }

        void requestClose() noexcept override
        {
            probe_->close_requested = true;
        }

        [[nodiscard]] bool closeComplete() const noexcept override
        {
            return probe_->close_requested && !probe_->lease;
        }

        [[nodiscard]] bool closeNeedsOwnerTick() const noexcept override
        {
            return probe_->close_requested && bool(probe_->lease);
        }

        [[nodiscard]] bool supportsDynamicRemoval() const noexcept override
        {
            return true;
        }

    private:
        CrossPhaseBlobCloseProbe* probe_{};
    };

    [[nodiscard]] lux::runtime::SceneContributionDescriptor
    makeCrossPhaseBlobCloseContribution(
        CrossPhaseBlobCloseProbe& probe)
    {
        lux::runtime::SceneContributionDescriptor descriptor;
        descriptor.id = lux::scene::SceneFeatureId{
            "org.test.scene.cross_phase_blob_close"};
        descriptor.display_name = "Cross-phase blob close probe";
        descriptor.config_schema_version = 0u;
        descriptor.provider = lux::extensions::ExtensionId{
            "org.test.cross_phase_blob_close"};
        descriptor.build = [&probe](
            lux::runtime::SceneContributionBatchBuilder& builder,
            const lux::runtime::SceneContributionBuildContext&,
            lux::runtime::ContributionConfig)
            -> lux::cxx::expected<
                void,
                lux::runtime::SceneContributionBuildFailure>
        {
            return builder.add(
                std::make_unique<CrossPhaseBlobCloseSystem>(probe),
                lux::ecs::kPhaseSimulation);
        };
        return descriptor;
    }

    class NonTerminalRollbackIntegration final
        : public lux::runtime::ISceneRuntimeIntegration
    {
    public:
        [[nodiscard]] lux::cxx::TypeToken type() const noexcept override
        {
            return lux::cxx::typeToken<NonTerminalRollbackIntegration>();
        }

        [[nodiscard]] lux::cxx::expected<
            void,
            lux::runtime::ESceneIntegrationError>
        prepare(lux::runtime::SceneRuntimeAssemblyContext&) noexcept override
        {
            return {};
        }

        [[nodiscard]] lux::cxx::expected<
            void,
            lux::runtime::ESceneIntegrationError>
        finalize(lux::runtime::SceneRuntimeAssemblyContext&) noexcept override
        {
            return {};
        }

        [[nodiscard]] lux::cxx::expected<
            void,
            lux::runtime::ESceneIntegrationError>
        onPublished(
            lux::runtime::SceneRuntimePublishedContext&) noexcept override
        {
            return lux::cxx::unexpected(
                lux::runtime::ESceneIntegrationError::PUBLICATION_FAILED);
        }

        void processSafePoint() noexcept override {}

        [[nodiscard]] lux::runtime::ESceneIntegrationCloseStatus close()
            noexcept override
        {
            return lux::runtime::ESceneIntegrationCloseStatus::RETRY_REQUIRED;
        }

    };

    [[nodiscard]] int runNonTerminalBringUpRollbackChild()
    {
        lux::asset::AssetManager assets{
            lux::asset::runtimeAssetCodecCatalog()};
        lux::exec::AsyncRuntimeBuilder builder;
        auto asset_service_result =
            lux::asset_runtime::AssetLoadService::addTo(builder, assets);
        auto section_service_result =
            lux::runtime::entity_scene::EntitySectionService::addTo(builder);
        if (!asset_service_result || !section_service_result)
            return 0;
        auto runtime_plan = std::move(builder).compile();
        if (!runtime_plan)
            return 0;

        auto asset_service = std::move(*asset_service_result);
        auto section_service = std::move(*section_service_result);
        lux::exec::AsyncRuntime async{std::move(*runtime_plan)};
        lux::ecs::ComponentTypeCatalog components;
        lux::runtime::SceneRuntime::Dependencies dependencies{
            assets,
            asset_service.client(),
            async,
            components,
            section_service.loadClient()};
        lux::runtime::SceneRuntime::Config config;
        config.name = "Non-terminal bring-up rollback";
        lux::scene::SceneDescription description;
        description.id =
            uuid("10000000-0000-4000-8000-000000000003");
        config.scene_asset_id = registerScene(
            assets, std::move(description));

        auto scene = lux::runtime::SceneRuntime::create(
            dependencies,
            config,
            std::make_unique<NonTerminalRollbackIntegration>());

        // The fixed implementation aborts before reaching this cleanup. It is
        // deliberately complete so the pre-fix implementation exits zero and
        // the parent process detects the missing fail-closed lifetime gate.
        scene.reset();
        section_service.close();
        asset_service.close();
        (void)lux::exec::testing::closeRuntime(async);
        return 0;
    }
}

int main(int argc, char** argv)
{
    namespace format = lux::ecs::scene_format;
    namespace scene = lux::scene;

    constexpr std::string_view rollback_child_argument =
        "--non-terminal-bring-up-rollback-child";
    if (argc == 2 && std::string_view{argv[1]} == rollback_child_argument)
        return runNonTerminalBringUpRollbackChild();

    const std::string rollback_command = std::string{"\""} + argv[0] +
        "\" " + std::string{rollback_child_argument};
    const int rollback_result = std::system(rollback_command.c_str());
    check(
        rollback_result != -1 && rollback_result != 0,
        "non-terminal bring-up rollback fails closed before owner release");

    format::EntitySectionImage section;
    section.section = format::EntitySectionId{
        uuid("20000000-0000-4000-8000-000000000001")};
    section.component_names = {""};
    section.archetypes.push_back({});
    const lux::ecs::PersistentEntityId root_id{
        uuid("30000000-0000-4000-8000-000000000001")};
    section.entities = {
        {0u, root_id},
        {0u, std::nullopt}};
    section.parents.push_back({1u, 0u});
    format::EntitySectionAttachment close_attachment;
    close_attachment.reference.type =
        format::ContentTypeId{"org.test.scene.close_blob"};
    close_attachment.reference.schema_version = 1u;
    close_attachment.payload = {
        std::byte{1u}, std::byte{3u}, std::byte{5u}};
    close_attachment.reference.id = format::makeContentBlobId(
        close_attachment.reference.type,
        close_attachment.reference.schema_version,
        close_attachment.payload);
    const auto close_blob_reference = close_attachment.reference;
    section.attachments.push_back(std::move(close_attachment));
    auto section_bytes = format::encodeEntitySectionImage(section);
    check(section_bytes.has_value(), "LXES startup Section encodes");
    if (!section_bytes)
        return 1;

    scene::SectionRecord record;
    record.id = section.section;
    record.source = scene::StoredSectionSource{"/Game/startup_lxes"};
    record.content_digest = format::entitySectionContentDigest(*section_bytes);
    record.encoded_bytes = section_bytes->size();
    record.decoded_bytes = section_bytes->size();
    record.entity_count = 2u;

    scene::SceneDescription package;
    package.id = lux::asset::asset_id_t{
        uuid("10000000-0000-4000-8000-000000000001")};
    package.startup_sections.push_back(record.id);
    package.sections.push_back(record);
    package.features.push_back({
        lux::scene::SceneFeatureId{
            "org.test.scene.cross_phase_blob_close"},
        0u,
        {}});
    auto package_bytes = scene::SceneAssetSerDeser::encodeData(
        package.id,
        package);
    check(package_bytes.has_value(), "LXSC SceneDescription encodes");
    if (!package_bytes)
        return 1;

    auto vfs = std::make_shared<lux::asset::AssetVfs>();
    auto section_blob = lux::asset::AssetBlob::fromShared(
        lux::cxx::SharedBytes<>::copyOf(std::span<const std::byte>{
            section_bytes->data(), section_bytes->size()}));
    const auto mount = vfs->mount({
        "/Game",
        std::make_shared<SectionProvider>(
            uuid("40000000-0000-4000-8000-000000000001"),
            "startup_lxes",
            std::move(section_blob)),
        0});
    check(mount != lux::asset::kInvalidMountId, "LXES provider mounts");

    lux::asset::AssetManager assets{
        lux::asset::runtimeAssetCodecCatalog()};
    lux::exec::AsyncRuntimeBuilder builder;
    auto asset_service_result =
        lux::asset_runtime::AssetLoadService::addTo(builder, assets);
    auto section_service_result =
        lux::runtime::entity_scene::EntitySectionService::addTo(builder);
    check(asset_service_result.has_value(), "Asset service assembles");
    check(section_service_result.has_value(), "EntitySection service assembles");
    if (!asset_service_result || !section_service_result)
        return 1;
    auto runtime_plan = std::move(builder).compile();
    check(runtime_plan.has_value(), "headless AsyncRuntime graph compiles");
    if (!runtime_plan)
        return 1;

    auto asset_service = std::move(*asset_service_result);
    auto section_service = std::move(*section_service_result);
    lux::exec::AsyncRuntime async{std::move(*runtime_plan)};
    RuntimeCloseProbe runtime_close_probe;
    CrossPhaseBlobCloseProbe cross_phase_blob_close_probe;
    lux::runtime::SceneContributionCatalog contribution_catalog;
    check(
        contribution_catalog.add(makeRuntimeCloseContribution(
            async, runtime_close_probe)).has_value(),
        "SceneRuntime close probe contribution registers");
    check(
        contribution_catalog.add(makeCrossPhaseBlobCloseContribution(
            cross_phase_blob_close_probe)).has_value(),
        "cross-phase blob close contribution registers");
    lux::ecs::ComponentTypeCatalog components;
    lux::runtime::SceneRuntime::Dependencies dependencies{
        assets,
        asset_service.client(),
        async,
        components,
        section_service.loadClient()};
    dependencies.scene_contribution_catalog = &contribution_catalog;
    lux::runtime::SceneRuntime::Config config;
    config.name = "LXSC Headless Scene";
    config.scene_origin = "/Game/scene_lxsc";
    auto decoded_package = scene::SceneAssetSerDeser::decodeData(
        std::span<const std::byte>{
            package_bytes->data(), package_bytes->size()});
    check(decoded_package.has_value(), "wrapped SceneAsset decodes");
    if (!decoded_package)
        return 1;
    config.scene_asset_id = registerScene(
        assets, std::move(**decoded_package));
    config.section_vfs = vfs;

    auto scene_runtime = lux::runtime::SceneRuntime::create(dependencies, config);
    check(scene_runtime != nullptr, "headless SceneRuntime accepts LXSC");
    if (!scene_runtime)
        return 1;
    check(
        scene_runtime->state() == lux::runtime::ESceneRuntimeState::LOADING,
        "startup Sections begin in LOADING");
    check(
        scene_runtime->world().registry().view<entt::entity>().empty(),
        "live registry is unchanged before the command barrier");

    lux::input::ActionMapper input;
    {
        lux::exec::testing::CloseEpoch startup{async};
        startup.driveWithStep(
            [&]() noexcept
            {
                scene_runtime->tick(0.0f, 0.0f, 0.0f, input);
            },
            [&]() noexcept
            {
                return scene_runtime->state() !=
                    lux::runtime::ESceneRuntimeState::LOADING;
            },
            [&scene_runtime]() noexcept
            {
                const auto state =
                    scene_runtime->entitySectionLoaderSnapshot();
                return state.waiting_admission_sections != 0u ||
                    state.staging_sections != 0u ||
                    state.armed_sections != 0u ||
                    (scene_runtime->state() ==
                         lux::runtime::ESceneRuntimeState::LOADING &&
                     state.waiting_sections == 0u &&
                     state.staging_sections == 0u &&
                     state.armed_sections == 0u);
            });
    }

    check(
        scene_runtime->isReady(),
        "all startup Sections commit before READY");
    auto& registry = scene_runtime->world().registry();
    check(
        registry.view<entt::entity>().size() == 2u,
        "LXES entities publish exactly once");
    auto* persistent =
        scene_runtime->services().get<lux::ecs::PersistentEntityIndex>();
    const auto root = persistent ? persistent->find(root_id) : entt::null;
    const auto children = registry.view<lux::ecs::ParentComponent>();
    check(
        root != entt::null && children.size() == 1u &&
            children.get<lux::ecs::ParentComponent>(*children.begin())
                    .parent() == root,
        "batch-local parent ordinal relocates at the command barrier");
    auto* const content_blobs = scene_runtime->services().get<
        lux::runtime::entity_scene::ContentBlobClient>();
    check(content_blobs != nullptr, "scene publishes ContentBlobClient");
    std::shared_ptr<
        lux::runtime::entity_scene::ContentBlobLease>
        delayed_scope_blob;
    bool delayed_scope_blob_released = false;
    if (content_blobs)
    {
        auto close_blob_lease = content_blobs->resolve(
            close_blob_reference);
        check(
            close_blob_lease.has_value(),
            "later-phase close consumer pins a startup Section blob");
        if (close_blob_lease)
        {
            cross_phase_blob_close_probe.lease =
                std::move(*close_blob_lease);
        }

        auto scope_blob_lease = content_blobs->resolve(
            close_blob_reference);
        check(
            scope_blob_lease.has_value(),
            "scene AsyncScope preparation pins a startup Section blob");
        if (scope_blob_lease)
        {
            delayed_scope_blob = std::make_shared<
                lux::runtime::entity_scene::ContentBlobLease>(
                    std::move(*scope_blob_lease));
            const auto release_scope_blob =
                [delayed_scope_blob,
                 &delayed_scope_blob_released]() noexcept
                {
                    *delayed_scope_blob = {};
                    delayed_scope_blob_released = true;
                };
            auto delayed_release = stdexec::schedule(
                    lux::exec::mainThreadScheduler(async))
                | stdexec::then(release_scope_blob)
                | stdexec::upon_stopped(release_scope_blob);
            check(
                lux::exec::spawn(
                    scene_runtime->asyncScope(), std::move(delayed_release)),
                "scene AsyncScope admits delayed blob release");
        }
    }
    std::size_t scene_close_completions = 0u;
    const auto scene_close = closeScene(
        *scene_runtime, async, scene_close_completions);
    check(
        scene_close.clean() && scene_close_completions == 1u,
        "LXSC Scene closes exactly once through the same barrier");
    check(
        cross_phase_blob_close_probe.close_requested &&
            cross_phase_blob_close_probe.released_on_simulation_tick &&
            !cross_phase_blob_close_probe.lease,
        "Scene close advances later phases until Section blob owners retire");
    check(
        delayed_scope_blob_released && delayed_scope_blob &&
            !*delayed_scope_blob,
        "scene scope terminal wakes Section close after delayed blob release");
    scene_runtime.reset();

    lux::runtime::SceneRuntime::Config ordered_close_config;
    ordered_close_config.name = "Domain-neutral ordered close";
    scene::SceneDescription ordered_close_description{
        lux::asset::asset_id_t{
            uuid("10000000-0000-4000-8000-000000000004")}};
    ordered_close_description.features.push_back({
        lux::scene::SceneFeatureId{
            "org.test.scene.runtime_close"},
        0u,
        {}});
    ordered_close_config.scene_asset_id = registerScene(
        assets, std::move(ordered_close_description));
    auto ordered_close_scene = lux::runtime::SceneRuntime::create(
        dependencies, ordered_close_config);
        check(
            ordered_close_scene != nullptr &&
                runtime_close_probe.child_admitted &&
                runtime_close_probe.contribution_service_alive,
            "SceneRuntime installs an externally-owned close scope");
    if (ordered_close_scene)
    {
        const auto persistence = ordered_close_scene->persistenceSnapshot(
            std::span<const std::string>{});
        check(
            ordered_close_scene->hasActiveContribution(
                "org.test.scene.runtime_close") &&
                persistence.features.size() == 1u &&
                persistence.features.front().id.name() ==
                    "org.test.scene.runtime_close",
            "manifest contribution is adopted as the authoritative persistent root");
        std::size_t completions = 0u;
        const auto report = closeScene(
            *ordered_close_scene, async, completions);
        check(
            report.clean() && completions == 1u &&
                runtime_close_probe.child_terminal &&
                runtime_close_probe.scope_terminal,
            "SceneRuntime waits for an installed system's AsyncScope");
        check(
            runtime_close_probe.trace ==
                    std::vector<int>{20, 10, -20, -10} &&
                !runtime_close_probe.system_destroyed_after_service &&
                !runtime_close_probe.contribution_service_alive,
            "SceneRuntime destroys systems before contribution services");
        ordered_close_scene.reset();
    }

    lux::runtime::SceneRuntime::Config empty_config;
    empty_config.name = "Dispatcher-independent close";
    scene::SceneDescription empty_description;
    empty_description.id =
        uuid("10000000-0000-4000-8000-000000000002");
    empty_config.scene_asset_id = registerScene(
        assets, std::move(empty_description));
    auto closing_scene = lux::runtime::SceneRuntime::create(
        dependencies, empty_config);
    check(closing_scene != nullptr, "empty headless Scene assembles");

    section_service.close();
    asset_service.close();
    (void)lux::exec::testing::closeRuntime(async);
    if (closing_scene)
    {
        std::size_t completions = 0u;
        const auto report = closeScene(
            *closing_scene, async, completions);
        check(
            completions == 1u && report.clean() &&
                !closing_scene->isLive(),
            "Scene close is exact-once after dispatcher admission closes");
        closing_scene.reset();
    }

    return failures == 0 ? 0 : 1;
}
