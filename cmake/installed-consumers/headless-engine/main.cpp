#include <lux/engine/EngineMetaFunctionRegistry.hpp>
#include <lux/engine/Init.hpp>
#include <lux/engine/SceneBuilder.hpp>
#include <lux/engine/SceneSystemRegistration.hpp>
#include <lux/engine/ecs/Transform.hpp>
#include <lux/engine/ecs/TransformSchema.hpp>
#include <lux/engine/scene/SceneDescription.hpp>
#include <lux/engine/scene/SceneInstance.hpp>
#include <lux/engine/scene/TransformSystem.hpp>
#include <lux/engine/world/World.hpp>

#include <chrono>

int main()
{
    using namespace lux;
    constexpr std::size_t budget = 4 * 1024 * 1024;
    const execution::ExecutionRuntimeConfig execution_config{
        .cpu_concurrency = 2, .timer = {16}, .blocking = execution::BlockingSchedulerConfig{1, 32}
    };
#if defined(LUX_ENGINE_WITH_RENDER)
    render::RenderServerConfig render_config;
    render_config.channel_name = "lux.installed.engine.assembly";
    auto context = engine::init(execution_config, {budget, budget, 32}, std::move(render_config), {});
#else
    auto context = engine::init(execution_config, {budget, budget, 32});
#endif
    if (!context)
    {
        return 1;
    }

    engine::Engine engine{**context};
    engine::EngineMetaFunctionRegistry metadata;
    for (const auto& schema : ecs::transformComponentSchemas())
    {
        if (!metadata.addComponent(schema))
        {
            return 2;
        }
    }
    auto transform = scene::transformSystemRegistration();
    auto config = scene::makeTransformSystemConfiguration(16, {64, 4096});
    if (!config)
    {
        return 3;
    }
    scene::SceneDescription description;
    description.world.name = "Installed scene";
    description.systems.push_back({
        transform.name, "transform", transform.configuration_version, std::move(*config)
    });
    if (!metadata.addSceneSystem(std::move(transform)))
    {
        return 4;
    }

    engine::Engine::SceneBuilder builder{engine, metadata};
    auto built = builder.build(description);
    if (!built)
    {
        return 5;
    }
    auto& registry = (*built)->world().registry();
    const auto entity = registry.create();
    const Eigen::Vector3d position{3, 5, 7};
    registry.emplace<ecs::Transform3D>(entity).translation = position;
    const auto id = engine.addScene(*built);
    if (!id || *built)
    {
        return 6;
    }

    bool transformed = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    engine.addEngineStageCallback([&]() noexcept -> engine::Engine::Result
    {
        if (const auto* world = registry.try_get<ecs::WorldTransform3D>(entity))
        {
            transformed = world->value.translation().isApprox(position);
            engine.requestStop();
        }
        else if (std::chrono::steady_clock::now() >= deadline)
        {
            engine.requestStop();
        }
        return {};
    }, engine::EEngineStage::END_UPDATE);
    return engine.run() && transformed ? 0 : 7;
}
