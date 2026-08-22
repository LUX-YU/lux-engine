#include <lux/engine/ecs/ScheduleBuilder.hpp>
#include <lux/engine/ecs/SceneServices.hpp>
#include <lux/engine/ecs/World.hpp>
#include <lux/engine/extensions/physics2d/Physics2D.hpp>
#include <lux/engine/runtime/extensions/ExtensionModuleManager.hpp>

#include <cstdio>
#include <filesystem>
#include <utility>
#include <vector>

int main(int argc, char** argv)
{
    if (argc != 2)
        return 2;

    int failures = 0;
    const auto check = [&failures](bool value, const char* message)
    {
        std::printf("[%s] %s\n", value ? " ok " : "FAIL", message);
        failures += value ? 0 : 1;
    };

    using namespace lux::extensions;
    ExtensionModuleManager modules;
    auto prepared = ExtensionModuleManager::prepare(
        ExtensionModuleRequirement::fromPath(
            ExtensionId{"org.lux.physics2d"},
            std::filesystem::path{argv[1]},
            EExtensionModuleTarget::RUNTIME,
            1u,
            0u));
    check(prepared.has_value(), "Physics2D v5 module validates");
    if (!prepared)
        return 1;

    std::vector<PreparedExtensionModule> batch;
    batch.push_back(std::move(*prepared));
    auto committed = modules.commitBatch(std::move(batch));
    check(committed.has_value(), "Physics2D v5 module commits");
    if (!committed)
        return 1;

    auto entrypoints = modules.entrypoints(
        extensionId("org.lux.physics2d"));
    check(
        entrypoints.world_systems != nullptr &&
            entrypoints.render_features == nullptr &&
            entrypoints.editor_panels == nullptr,
        "Physics2D exposes only direct World-system installation");

    {
        lux::ecs::SceneServices services;
        lux::ecs::World world;
        lux::ecs::Schedule schedule{world};
        lux::ecs::ScheduleBuilder builder{schedule, services};
        const auto installed = entrypoints.world_systems(builder);
        check(
            static_cast<bool>(installed),
            "v5 entrypoint stages systems without a contribution catalog");
        check(
            builder.pendingCount() == 1u &&
                builder.services().contains<physics2d::PhysicsWorldApi>(),
            "Physics2D stages one system and its typed World service");
        const auto published = builder.commit();
        check(
            published.has_value() && schedule.compile().valid() &&
                schedule.systemCount() == 1u &&
                services.contains<physics2d::PhysicsWorldApi>(),
            "Physics2D publishes through the ordinary Schedule transaction");
    }

    entrypoints = {};
    committed->clear();
    check(
        modules.close().has_value(),
        "Physics2D unloads after World bindings and external leases release");
    return failures == 0 ? 0 : 1;
}
