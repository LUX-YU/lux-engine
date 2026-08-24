#include <lux/engine/ecs/render/presentation/PrimaryViewPresentation.hpp>
#include <lux/engine/ecs/render/presentation/PrimaryViewPresentationSystem.hpp>

#include <lux/engine/ecs/SceneServices.hpp>
#include <lux/engine/ecs/Schedule.hpp>
#include <lux/engine/ecs/ScheduleBuilder.hpp>
#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/render/components/PrimaryCameraTag.hpp>
#include <lux/engine/ecs/render/components/ViewPresentComponent.hpp>

#include <cstdio>
#include <memory>

namespace
{
    int failures = 0;

    void expect(bool condition, const char* message)
    {
        std::fprintf(
            stderr,
            "[%s] %s\n",
            condition ? " ok " : "FAIL",
            message);
        failures += condition ? 0 : 1;
    }
}

int main()
{
    lux::ecs::World world;
    lux::ecs::Schedule schedule{world};
    lux::ecs::SceneServices services;
    lux::ecs::render::presentation::PrimaryViewPresentation presentation{
        true,
        lux::render::RenderTargetId{3u, 1u},
        lux::math::Extent2u{1280u, 720u}};

    lux::ecs::ScheduleBuilder builder{schedule, services};
    const auto system = builder.add(
        std::make_unique<
            lux::ecs::render::presentation::
                PrimaryViewPresentationSystem>(
                presentation),
        lux::ecs::kPhasePreTransform);
    expect(system.has_value(), "primary presentation system stages");
    expect(builder.commit().has_value(), "primary presentation system commits");

    auto& registry = world.registry();
    const auto first = registry.create();
    registry.emplace<lux::ecs::PrimaryCameraTag>(first);
    expect(
        !registry.all_of<lux::ecs::ViewPresentComponent>(first),
        "folded primary camera is unchanged before the command barrier");

    schedule.tick(0.0f);
    const auto* first_view =
        registry.try_get<lux::ecs::ViewPresentComponent>(first);
    expect(
        first_view && first_view->target ==
                lux::render::RenderTargetId{3u, 1u} &&
            first_view->extent.width == 1280u &&
            first_view->extent.height == 720u,
        "the unique primary camera binds exactly at the barrier");
    expect(
        presentation.snapshot().status ==
                lux::ecs::render::presentation::
                    EPrimaryViewPresentationStatus::BOUND &&
            presentation.snapshot().candidate_count == 1u &&
            presentation.snapshot().bound_camera == first &&
            !presentation.snapshot().command_pending,
        "bound state is structurally observable");

    const auto first_commit =
        presentation.snapshot().committed_revision;
    presentation.setOutputIntent(
        lux::render::RenderTargetId{7u, 2u},
        lux::math::Extent2u{1920u, 1080u});
    expect(
        registry.get<lux::ecs::ViewPresentComponent>(first).target ==
            lux::render::RenderTargetId{3u, 1u},
        "output intent does not patch the registry synchronously");
    schedule.tick(0.0f);
    expect(
        registry.get<lux::ecs::ViewPresentComponent>(first).target ==
                lux::render::RenderTargetId{7u, 2u} &&
            presentation.snapshot().committed_revision == first_commit + 1u,
        "reattach intent commits once through the command barrier");
    schedule.tick(0.0f);
    expect(
        presentation.snapshot().committed_revision == first_commit + 1u,
        "unchanged intent does not enqueue another command");

    const auto second = registry.create();
    registry.emplace<lux::ecs::PrimaryCameraTag>(second);
    schedule.tick(0.0f);
    expect(
        presentation.snapshot().status ==
                lux::ecs::render::presentation::
                    EPrimaryViewPresentationStatus::
                    AMBIGUOUS_PRIMARY_CAMERA &&
            presentation.snapshot().candidate_count == 2u &&
            presentation.snapshot().bound_camera == entt::null &&
            !registry.all_of<lux::ecs::ViewPresentComponent>(first) &&
            !registry.all_of<lux::ecs::ViewPresentComponent>(second),
        "ambiguous primary content fails closed and removes the owned view");

    registry.remove<lux::ecs::PrimaryCameraTag>(second);
    schedule.tick(0.0f);
    expect(
        presentation.snapshot().status ==
                lux::ecs::render::presentation::
                    EPrimaryViewPresentationStatus::BOUND &&
            presentation.snapshot().bound_camera == first &&
            registry.all_of<lux::ecs::ViewPresentComponent>(first),
        "resolving ambiguity rebinds the unique camera");

    registry.remove<lux::ecs::PrimaryCameraTag>(first);
    schedule.tick(0.0f);
    expect(
        presentation.snapshot().status ==
                lux::ecs::render::presentation::
                    EPrimaryViewPresentationStatus::
                    NO_PRIMARY_CAMERA &&
            presentation.snapshot().candidate_count == 0u &&
            presentation.snapshot().bound_camera == entt::null &&
            !registry.all_of<lux::ecs::ViewPresentComponent>(first),
        "zero primary cameras produces no view");

    return failures == 0 ? 0 : 1;
}
