#include <lux/engine/ecs/HierarchyIndex.hpp>
#include <lux/engine/ecs/PersistentEntity.hpp>
#include <lux/engine/ecs/Schedule.hpp>
#include <lux/engine/ecs/Transform.hpp>
#include <lux/engine/ecs/TransformSystem.hpp>
#include <lux/engine/ecs/WorldSnapshot.hpp>
#include <lux/engine/resource/asset/AssetCodecSet.hpp>
#include <lux/engine/resource/asset/AssetId.hpp>

#include <memory>
#include <utility>

int main()
{
    lux::asset::AssetId asset_id;
    lux::asset::AssetCodecSet codecs;
    if (!asset_id.isNull() || !codecs.descriptors().empty())
        return 1;

    lux::ecs::World world;
    lux::ecs::HierarchyIndex hierarchy(world);
    auto edit_result = world.edit();
    if (!edit_result)
        return 2;
    auto edit = std::move(*edit_result);
    const auto entity = edit.create();
    edit.emplace<lux::ecs::Transform3D>(entity);
    edit = {};

    lux::ecs::Schedule schedule(world);
    auto schedule_edit_result = schedule.edit();
    if (!schedule_edit_result)
        return 3;
    auto schedule_edit = std::move(*schedule_edit_result);
    const auto transform = schedule_edit.add(
        std::make_unique<lux::ecs::Transform3DSystem>(hierarchy)
    );
    if (!transform || !schedule_edit.commit())
        return 4;
    schedule.run(1.0F / 60.0F, 1u);
    return world.find<lux::ecs::WorldTransform3D>(entity) == nullptr ? 5 : 0;
}
