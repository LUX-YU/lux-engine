#include <lux/engine/function/render/client/core/RenderFeatureMetaModule.hpp>
#include <lux/engine/scene/Builtin3DRenderIntegration.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/RenderSystemConfiguration.hpp>
#include <lux/engine/scene/SceneRenderSchema.hpp>
#include <lux/engine/scene/Scene.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/ecs/TransformSchema.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>
#include <lux/engine/simulation/ecs/VisualSchema.hpp>
#include <lux/engine/world/WorldDescriptionBuilder.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

namespace
{
    class FakeRenderRuntime final : public lux::scene::RenderRuntime
    {
    public:
        FakeRenderRuntime()
            : control_channel_(lux::render::RenderControlChannel<>::create(32U)),
              program_channel_(lux::render::RenderProgramChannel<>::create(2U)),
              sync_(std::make_shared<lux::render::RenderChannelSync>()),
              control_(control_channel_, sync_),
              programs_(program_channel_, sync_),
              responder_([this](std::stop_token stop) noexcept { respond(stop); })
        {
        }

        ~FakeRenderRuntime() noexcept override
        {
            responder_.request_stop();
            sync_->requestStop();
            responder_.join();
        }

        [[nodiscard]] lux::cxx::expected<lux::scene::RenderRuntimeLease, lux::scene::RenderRuntimeFailure>
        acquire() noexcept override
        {
            if (stopping_)
            {
                return lux::cxx::unexpected(lux::scene::RenderRuntimeFailure{
                    lux::scene::ERenderRuntimeError::STOPPING
                });
            }
            ++demand_;
            return makeLease();
        }

        [[nodiscard]] std::size_t demand() const noexcept
        {
            return demand_;
        }

        [[nodiscard]] std::size_t createdScenes() const noexcept
        {
            return created_scenes_.load(std::memory_order_acquire);
        }

        [[nodiscard]] std::size_t destroyedScenes() const noexcept
        {
            return destroyed_scenes_.load(std::memory_order_acquire);
        }

    private:
        template <class Reply>
        void publish(lux::render::TypeId type, lux::render::RequestId request, const Reply& reply) noexcept
        {
            lux::render::ReplyPacket<>* slot{};
            while (slot == nullptr && !sync_->isStopping())
            {
                slot = control_channel_->responses.tryBeginWrite();
                if (slot == nullptr)
                {
                    std::this_thread::yield();
                }
            }
            if (slot == nullptr)
            {
                return;
            }
            slot->clear_keep_capacity();
            slot->payload.resize(sizeof(Reply));
            std::memcpy(slot->payload.data(), std::addressof(reply), sizeof(Reply));
            slot->replies.push_back(lux::render::ReplyRecord{
                .type_id = type,
                .payload_offset = 0U,
                .payload_size = sizeof(Reply),
                .request_id = request
            });
            assert(control_channel_->responses.publishWrite());
            sync_->notifyReplyProduced();
        }

        void respond(std::stop_token stop) noexcept
        {
            lux::render::OperationPacket<> packet;
            std::uint32_t feature_index{1U};
            while (!stop.stop_requested() && !sync_->isStopping())
            {
                if (control_channel_->requests.tryPop(packet) != lux::cxx::EQueuePopResult::VALUE)
                {
                    const auto observed = sync_->work_epoch.load(std::memory_order_acquire);
                    if (!stop.stop_requested() && !sync_->isStopping())
                    {
                        sync_->work_epoch.wait(observed, std::memory_order_acquire);
                    }
                    continue;
                }
                sync_->notifyRequestStateChanged();
                if (packet.operationId() == lux::render::type_ids::CreateScene)
                {
                    created_scenes_.fetch_add(1U, std::memory_order_release);
                    publish(
                        lux::render::type_ids::ReplySceneCreated,
                        packet.requestId(),
                        lux::render::SceneCreatedReply{lux::render::RenderSceneId{1U, 1U}, {}}
                    );
                }
                else if (packet.operationId() == lux::render::type_ids::AddFeature)
                {
                    publish(
                        lux::render::type_ids::ReplyFeatureAdded,
                        packet.requestId(),
                        lux::render::FeatureAddedReply{lux::render::FeatureHandle{feature_index++, 1U}, {}}
                    );
                }
                else if (packet.operationId() == lux::render::type_ids::DestroyScene)
                {
                    destroyed_scenes_.fetch_add(1U, std::memory_order_release);
                }
                packet.clear_keep_capacity();
            }
        }

        void release() noexcept override
        {
            (void)control_.flushDeferredReleases();
            assert(demand_ != 0U);
            --demand_;
        }

        lux::render::RenderControlSession& control() noexcept override
        {
            return control_;
        }

        lux::render::RenderProgramSession& programs() noexcept override
        {
            return programs_;
        }

        lux::render::RenderUploadClient upload() noexcept override
        {
            return {};
        }

        const lux::render::FeatureCatalog& features() const noexcept override
        {
            return catalog_;
        }

        std::shared_ptr<lux::render::RenderControlChannel<>> control_channel_;
        std::shared_ptr<lux::render::RenderProgramChannel<>> program_channel_;
        std::shared_ptr<lux::render::RenderChannelSync> sync_;
        lux::render::RenderControlSession control_;
        lux::render::RenderProgramSession programs_;
        lux::render::FeatureCatalog catalog_;
        std::jthread responder_;
        std::size_t demand_{};
        bool stopping_{};
        std::atomic_size_t created_scenes_{};
        std::atomic_size_t destroyed_scenes_{};
    };

    template <class Type>
    [[nodiscard]] Type worldId(std::uint8_t tail)
    {
        std::array<std::uint8_t, 16> bytes{};
        bytes[15] = tail;
        return Type{uuids::uuid(bytes)};
    }

    [[nodiscard]] lux::asset::AssetId assetId(std::uint8_t tail)
    {
        std::array<std::uint8_t, 16> bytes{};
        bytes[15] = tail;
        return lux::asset::AssetId(bytes);
    }

    [[nodiscard]] std::shared_ptr<const lux::world::WorldDescription> makeWorld()
    {
        lux::world::WorldDescriptionBuilder builder;
        assert(builder.setIdentity(
            worldId<lux::world::WorldBundleId>(1U),
            worldId<lux::world::WorldBundleGeneration>(2U),
            "render-system-test"
        ));
        assert(builder.setPartitioner({lux::world::worldPartitionerId("test.none"), 1U}, 0U));
        auto built = std::move(builder).build();
        assert(built);
        return std::make_shared<lux::world::WorldDescription>(std::move(*built));
    }

    [[nodiscard]] std::shared_ptr<const lux::simulation::SimulationDescription> makeSimulation()
    {
        lux::simulation::SimulationDescriptionBuilder builder;
        auto built = std::move(builder).build();
        assert(built);
        return std::make_shared<lux::simulation::SimulationDescription>(std::move(*built));
    }

    [[nodiscard]] std::shared_ptr<const lux::scene::SceneDescription> makeSceneDescription(bool render)
    {
        lux::scene::SceneDescriptionBuilder builder;
        builder.setWorld(assetId(1U));
        builder.setSimulation(assetId(2U));
        if (render)
        {
            const auto registration = lux::scene::builtinRenderSystemRegistration();
            lux::scene::RenderSystemConfiguration configuration;
            std::vector<std::byte> encoded;
            assert(registration.configuration.encode(&configuration, encoded));
            constexpr lux::system::SystemInstanceId Instance{1U};
            assert(builder.addSystem(
                Instance,
                "render",
                registration.type,
                registration.description->version,
                registration.description->configuration_schema_name,
                registration.description->configuration_schema_version,
                encoded
            ));
            assert(builder.bindRequirement(Instance, "render_runtime", "host.render"));
        }
        auto built = std::move(builder).build();
        assert(built);
        return std::make_shared<lux::scene::SceneDescription>(std::move(*built));
    }
}

int main()
{
    using namespace lux;

    render::initializeBuiltinRenderFeatureMeta();
    scene::initializeBuiltinRenderSystemMeta();
    meta::ReflectionRegistry::initRegistry();
    std::vector<simulation::ecs::ComponentSchema> schemas;
    const auto transform_schemas = simulation::ecs::transformComponentSchemas();
    const auto visual_schemas = simulation::ecs::visualComponentSchemas();
    schemas.insert(schemas.end(), transform_schemas.begin(), transform_schemas.end());
    schemas.insert(schemas.end(), visual_schemas.begin(), visual_schemas.end());
    const auto render_schemas = scene::sceneRenderComponentSchemas();
    schemas.insert(schemas.end(), render_schemas.begin(), render_schemas.end());
    auto components = simulation::ecs::ComponentSchemaSet::build(std::move(schemas));
    assert(components);
    const auto builtin_features = render::builtinRenderFeatureRegistrations();
    auto manager = scene::SceneMetaManager::build({
        std::move(*components),
        simulation::SimulationSystemRegistry{},
        {scene::builtinRenderSystemRegistration()},
        {builtin_features.begin(), builtin_features.end()},
        {scene::builtinRenderFeatureSceneBindings().begin(), scene::builtinRenderFeatureSceneBindings().end()}
    });
    assert(manager);
    const auto render_meta = manager->getSystemMeta(scene::RenderSystem::Description.canonical_name);
    assert(render_meta && render_meta->domain == scene::ESystemDomain::SCENE);
    assert(render_meta->configuration_reflection != nullptr);
    const auto* mesh_schema = manager->getComponentMeta(cxx::typeToken<simulation::ecs::Mesh3D>());
    assert(mesh_schema != nullptr);
    const auto mesh_usages = manager->systemsUsingComponent(mesh_schema->id);
    assert(std::ranges::any_of(mesh_usages, [](const auto& usage) noexcept {
        return usage.system == system::systemTypeId(scene::RenderSystem::Description.canonical_name) &&
            usage.via_render_feature == render::featureId("lux.render.mesh_stack.v1");
    }));

    const auto world = makeWorld();
    const auto simulation = makeSimulation();
    auto headless = scene::Scene::create({makeSceneDescription(false), world, simulation, *manager, {}});
    assert(headless);
    assert(!(*headless)->hasCapability("lux.scene.render"));

    auto missing = scene::Scene::create({makeSceneDescription(true), world, simulation, *manager, {}});
    assert(!missing);
    assert(missing.error().scene_system.code == scene::ESceneSystemBuildError::INVALID_REQUIREMENT_BINDING);

    FakeRenderRuntime runtime;
    const auto provider = scene::makeSceneCapabilityProvider<scene::RenderRuntime>(
        "host.render",
        "lux.render.runtime",
        runtime
    );
    auto rendered = scene::Scene::create({
        makeSceneDescription(true),
        world,
        simulation,
        *manager,
        std::span(&provider, 1U)
    });
    assert(rendered);
    assert(runtime.demand() == 1U && runtime.createdScenes() == 1U);
    assert((*rendered)->hasCapability("lux.scene.render"));
    const auto* render_system = (*rendered)->findSceneSystem<scene::RenderSystem>();
    assert(render_system != nullptr && render_system->renderSceneId().isValid());
    assert((*rendered)->executeStablePoint());
    assert((*rendered)->executePresentation());
    rendered->reset();
    assert(runtime.demand() == 0U);
    for (std::size_t retry{}; retry < 1000U && runtime.destroyedScenes() == 0U; ++retry)
    {
        std::this_thread::yield();
    }
    assert(runtime.destroyedScenes() == 1U);
    headless->reset();
    meta::ReflectionRegistry::destroyRegistry();
    return 0;
}
