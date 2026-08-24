#include <lux/engine/editor/scene/controllers/EditorCamera2DController.hpp>
#include <lux/engine/editor/scene/controllers/EditorCamera3DController.hpp>

#include <lux/engine/ecs/render/components/2d/Camera2DComponent.hpp>
#include <lux/engine/ecs/render/components/2d/Camera2DCacheComponent.hpp>
#include <lux/engine/ecs/render/systems/2d/Camera2DSystem.hpp>

#include <cassert>

int main()
{
    lux::input::ActionMapper input;

    {
        lux::ecs::Registry registry;
        const auto camera = registry.create();
        auto& transform = registry.emplace<lux::ecs::Transform3DComponent>(
            camera);
        transform.position = {100'010.0, 19'998.0, -79'996.0};

        lux::editor::EditorCamera3DController controller;
        controller.attach(camera, registry);
        controller.tick(input, 0.0f);

        const auto& local = registry.get<lux::ecs::Transform3DComponent>(
            camera);
        assert((local.position == lux::math::Position3d{
            100'010.0, 19'998.0, -79'996.0}));
        const auto focus = controller.orbitTargetWorld();
        assert((focus && *focus == lux::math::Position3d{
            100'010.0, 19'998.0, -80'001.0}));
    }

    {
        lux::ecs::Registry registry;
        const auto camera = registry.create();
        auto& transform = registry.emplace<lux::ecs::Transform2DComponent>(
            camera);
        transform.position = {-250'008.0, 125'003.0};
        registry.emplace<lux::ecs::Camera2DComponent>(camera);

        lux::editor::EditorCamera2DController controller;
        controller.attach(camera, &registry);
        controller.tick(input, 0.0f, 1080.0f);

        const auto& local = registry.get<lux::ecs::Transform2DComponent>(
            camera);
        assert((local.position == lux::math::Position2d{
            -250'008.0, 125'003.0}));

        lux::ecs::Camera2DCacheComponent cache;
        cache.render_origin = local.position;
        const auto clicked = lux::ecs::screenToWorldPosition(
            cache,
            {100.0f, 100.0f},
            {75.0f, 25.0f});
        const lux::math::Position2d clicked_expected{
            local.position.x + 0.5,
            local.position.y - 0.5};
        assert(clicked && *clicked == clicked_expected);
    }
}
