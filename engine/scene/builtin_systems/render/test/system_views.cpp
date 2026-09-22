#include <lux/engine/function/render/features/BuiltinFeatures.hpp>
#include <lux/engine/function/render/features/genops/ViewCameraOperation.ops.hpp>
#include <lux/engine/render/RenderFeature.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/scene/Builtin3DRenderIntegration.hpp>
#include <lux/engine/scene/Camera.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/RenderSystemConfiguration.hpp>
#include <lux/engine/scene/RenderSystemMetadata.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/scene/SceneInstance.hpp>
#include <lux/engine/scene/SceneRenderSchema.hpp>
#include <lux/engine/serialization/Serialization.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/TransformSchema.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

namespace
{
struct ClockObservation final
{
    std::atomic<std::int64_t> elapsed{}, delta{};
    std::atomic<std::uint64_t> step{}, frames{};
    std::atomic<float> maintenance{};
};

class ClockProbe final : public lux::render::RenderFeature
{
  public:
    explicit ClockProbe(ClockObservation *observation) : RenderFeature({"ClockProbe"}), observed_(*observation)
    {
    }

    void onFrameBegin(const lux::render::FeatureFrameContext &) override
    {
        observed_.elapsed = renderScene().elapsedNanoseconds();
        observed_.delta = renderScene().deltaNanoseconds();
        observed_.step = renderScene().simulationStep();
        observed_.maintenance = renderScene().maintenanceTime();
        ++observed_.frames;
    }

  private:
    ClockObservation &observed_;
};

lux::render::Expected<lux::render::FeatureHandle> createProbe(void *scene, const void *data, std::size_t size)
{
    auto config = lux::render::decodeCommConfig<ClockObservation *>(data, size);
    if (!config)
    {
        return lux::cxx::unexpected(config.error());
    }
    return static_cast<lux::render::RenderScene *>(scene)->addFeature<ClockProbe>(*config);
}
} // namespace

int main()
{
    using namespace lux;
    using namespace std::chrono_literals;
    namespace ecs = simulation::ecs;
    meta::ReflectionRegistry::initRegistry();
    scene::initializeBuiltinRenderSystemMeta();
    render::initializeBuiltinRenderFeatureMeta();
    std::vector<ecs::ComponentSchema> components;
    for (const auto schemas : {scene::sceneRenderComponentSchemas(), ecs::transformComponentSchemas()})
    {
        components.insert(components.end(), schemas.begin(), schemas.end());
    }
    auto schema = ecs::ComponentSchemaSet::build(std::move(components));
    assert(schema);
    auto metadata = scene::SceneMetaManager::build(
        {.components = *schema, .scene_systems = {scene::builtinRenderSystemRegistration()}});
    assert(metadata);
    auto all_bindings = scene::builtinRenderFeatureSceneBindings();
    const auto binding =
        std::ranges::find(all_bindings, render::kViewCameraDescriptor.type, &scene::RenderFeatureSceneBinding::feature);
    assert(binding != all_bindings.end());
    auto built_metadata = scene::RenderSystemMetadata::build(*metadata, {render::kViewCameraRenderFeatureRegistration},
                                                             std::span(&*binding, 1));
    assert(built_metadata);
    auto render_metadata = std::make_shared<const scene::RenderSystemMetadata>(std::move(*built_metadata));
    render::RendererConfig renderer;
    renderer.validation = true;
    renderer.feature_factories = {render::kViewCameraFeatureFactory,
                                  render::makeSimpleFactory(createProbe, "ClockProbe")};
    auto made_runtime = render::RenderRuntime::create(std::move(renderer));
    assert(made_runtime);
    auto runtime = std::move(*made_runtime);
    scene::RenderSystemConfiguration config;
    config.features.push_back({render::kViewCameraDescriptor.type, {}});
    std::vector<std::byte> bytes;
    assert(scene::builtinRenderSystemRegistration().configuration.encode(&config, bytes));
    scene::SceneDescriptionBuilder builder;
    assert(builder.addSystem({1}, "render", scene::builtinRenderSystemRegistration().type, 1,
                             scene::RenderSystem::Description.configuration_schema_name, 1, bytes));
    auto built = std::move(builder).buildResolved();
    assert(built);
    std::array providers{
        scene::makeSceneCapabilityProvider<render::RenderRuntime>("runtime", "lux.render.runtime", *runtime),
        scene::makeSceneCapabilityProvider<std::shared_ptr<const scene::RenderSystemMetadata>>(
            "metadata", "lux.render.metadata", render_metadata)};
    scene::SceneCreateInfo create{std::make_shared<const scene::SceneDescription>(std::move(*built)),
                                  std::make_shared<const world::WorldDescription>(),
                                  std::make_shared<const simulation::SimulationDescription>(),
                                  *metadata,
                                  providers,
                                  simulation::ESimulationMode::EVOLUTION};
    auto made = scene::SceneInstance::create(create);
    assert(made);
    auto instance = std::move(*made);
    assert(instance->simulation().seal());
    auto *system = instance->findSceneSystem<scene::RenderSystem>();
    assert(system);
    auto receipt = system->resourceReceipt();
    auto executor = task::TaskExecutor::create({0, 1024});
    assert(executor);
    scene::SceneDriver driver(*executor);
    const auto poll = [&] {
        std::size_t controls = 4, programs = 1;
        assert(runtime->poll(16, controls, programs));
    };
    const auto until = [&](auto ready) {
        const auto end = std::chrono::steady_clock::now() + 15s;
        while (std::chrono::steady_clock::now() < end)
        {
            poll();
            if (ready())
            {
                return;
            }
            std::this_thread::sleep_for(1ms);
        }
        assert(false && "specified RenderSystem completion did not arrive");
    };
    until([&] { return receipt.status().state == render::ESceneResourceState::READY; });
    auto other = scene::SceneInstance::create(create);
    assert(other && (*other)->simulation().seal());
    auto *other_system = (*other)->findSceneSystem<scene::RenderSystem>();
    until([&] { return other_system->resourceStatus().state == render::ESceneResourceState::READY; });
    ClockObservation first_clock, second_clock;
    auto control = runtime->control();
    assert(control);
    const auto type = runtime->features().typeId("ClockProbe");
    assert(type);
    auto first_probe = control->get().addFeature(system->renderSceneId(), type, &first_clock);
    auto second_probe = control->get().addFeature(other_system->renderSceneId(), type, &second_clock);
    until([&] { return first_probe.isReady() && second_probe.isReady(); });
    assert(first_probe.tryResult() && second_probe.tryResult());
    assert(first_probe.tryResult()->get().error.ok() && second_probe.tryResult()->get().error.ok());
    auto first = system->openView({.extent = {200, 100}});
    auto second = system->openView({.extent = {100, 200}});
    assert(first && second);
    const auto first_id = (*first)->id();
    const auto second_id = (*second)->id();
    const auto camera = instance->registry().create();
    instance->registry().emplace<scene::Camera>(camera);
    instance->registry().emplace<ecs::WorldTransform3D>(camera);
    assert(!system->associateView(first_id, scene::SceneInstanceId{UINT64_MAX}, camera));
    assert(system->associateView(first_id, instance->id(), camera)); // Before create reply is adopted.
    assert(system->associateView(second_id, instance->id(), camera));
    until([&] {
        return (*first)->status().state == render::EViewState::READY &&
               (*second)->status().state == render::EViewState::READY;
    });
    scene::SceneAdvanceBudget budget{32, 1, 0};
    assert(driver.advance(*instance, std::chrono::steady_clock::now(), budget) == scene::ESceneProgress::PENDING);
    assert(system->transportStatistics().published == 1 && system->transportStatistics().pending == 1);
    instance->registry().patch<ecs::WorldTransform3D>(camera, [](auto &value) { value.value.translation().x() = 7; });
    until([&] {
        budget = {32, 1, 1};
        static_cast<void>(driver.advance(*instance, std::chrono::steady_clock::now(), budget));
        assert(instance->progress().result);
        return system->transportStatistics().forwarded == 2;
    });
    const auto before = system->transportStatistics().published;
    for (unsigned i{}; i != 8; ++i)
    {
        budget = {32, 1, 1};
        assert(driver.advance(*instance, std::chrono::steady_clock::now(), budget) == scene::ESceneProgress::COMPLETE);
    }
    assert(system->transportStatistics().published == before);
    // A real Driver step publishes time even without component changes. Its
    // pending Program does not execute the same delta twice.
    assert(driver.step(*instance));
    budget = {32, 1, 0};
    assert(driver.advance(*instance, std::chrono::steady_clock::now(), budget) == scene::ESceneProgress::PENDING);
    assert(instance->progress().clock.step_index == 1);
    for (unsigned i{}; i != 3; ++i)
    {
        budget = {32, 1, 0};
        assert(driver.advance(*instance, std::chrono::steady_clock::now(), budget) == scene::ESceneProgress::PENDING);
        assert(instance->progress().clock.step_index == 1);
    }
    budget = {32, 1, 1};
    assert(driver.advance(*instance, std::chrono::steady_clock::now(), budget) == scene::ESceneProgress::COMPLETE);
    budget = {32, 1, 1};
    assert(driver.advance(**other, std::chrono::steady_clock::now(), budget) == scene::ESceneProgress::COMPLETE);
    const auto draw = [&] {
        render::RenderProgram<> frame;
        render::RenderProgramBuilder<> builder(frame);
        builder.begin();
        frame.kind = render::ERenderProgramKind::Frame;
        const auto prior = runtime->statistics().frames;
        until([&] {
            const auto accepted = runtime->submit(frame);
            assert(accepted);
            return *accepted == render::EFrameSubmit::SUBMITTED;
        });
        until([&] { return runtime->statistics().frames > prior; });
    };
    draw();
    std::cerr << "backend clock=" << first_clock.step << ',' << first_clock.elapsed << ',' << first_clock.delta
              << " observations=" << first_clock.frames << ", second=" << second_clock.step << '\n';
    assert(first_clock.step == 1 && first_clock.elapsed == 16'000'000 && first_clock.delta == 16'000'000);
    assert(second_clock.step == 0 && second_clock.elapsed == 0 && second_clock.delta == 0);
    const auto maintenance = first_clock.maintenance.load();
    for (unsigned i{}; i != 3; ++i)
    {
        draw();
    }
    assert(first_clock.maintenance > maintenance && first_clock.step == 1 && second_clock.step == 0);
    assert((*second)->requestExtent({160, 90}));
    until([&] {
        budget = {32, 1, 1};
        static_cast<void>(driver.advance(*instance, std::chrono::steady_clock::now(), budget));
        return (*second)->status().state == render::EViewState::READY &&
               (*second)->status().ready_extent == (render::PixelExtent{160, 90}) &&
               system->transportStatistics().forwarded > before;
    });
    assert((*first)->status().ready_extent == (render::PixelExtent{200, 100}));
    first->reset();
    budget = {32, 1, 1};
    static_cast<void>(driver.advance(*instance, std::chrono::steady_clock::now(), budget));
    assert(!system->dissociateView(first_id, instance->id(), camera));
    assert(system->dissociateView(second_id, instance->id(), camera));
    assert(system->associateView(second_id, instance->id(), camera));
    instance->registry().destroy(camera);
    const auto replacement = instance->registry().create();
    instance->registry().emplace<scene::Camera>(replacement);
    budget = {32, 1, 1};
    static_cast<void>(driver.advance(*instance, std::chrono::steady_clock::now(), budget));
    assert(!system->associateView(second_id, instance->id(), camera));
    assert(!system->dissociateView(second_id, instance->id(), replacement));
    instance.reset();
    other->reset();
    second->reset();
    until([&] { return receipt.status().state == render::ESceneResourceState::RETIRED; });
    assert(receipt.status().failure.ok() && runtime->statistics().runtime_leases == 0);
    assert(runtime->statistics().validation_errors == 0);
    assert(runtime->beginClose());
    until([&] {
        std::size_t replies = 16, controls = 4, programs = 1;
        auto closed = runtime->advanceClose(replies, controls, programs);
        assert(closed);
        return *closed == render::ERenderClose::COMPLETE;
    });
    assert(runtime->joinStopped());
    std::cout << "PASS SceneInstance RenderSystem: independent View extents, instance/Entity generation, "
                 "pending publication, instance time/pause independent of real render maintenance, RAII retirement\n";
}
