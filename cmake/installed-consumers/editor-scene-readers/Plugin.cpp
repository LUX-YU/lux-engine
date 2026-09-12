#include "PluginApi.hpp"
#include "PluginComponent.hpp"
#if !defined(__LUX_PARSE_TIME__)
#include <consumer_scene_readers.inspector.generated.hpp>
#endif
namespace
{
    unsigned draws{};
    lux::simulation::ecs::Entity populate(lux::simulation::ecs::Registry &registry)
    {
        const auto entity = registry.create();
        registry.emplace<consumer::RichComponent>(entity);
        return entity;
    }
    bool verify(lux::editor::sessions::SceneSession &session, lux::editor::sessions::SceneEntityRef target)
    {
        const auto copy = session.readComponent<consumer::RichComponent>(target);
        return copy && copy->caption == "Plugin component" && copy->enabled && copy->hidden == 17 &&
            copy->signed_value == -1234567890123 && copy->unsigned_value == 12345678901234 &&
            copy->position == Eigen::Vector3d(1, 2, 3);
    }
    void draw(lux::editor::sessions::SceneSession &session, lux::editor::sessions::SceneEntityRef target,
        lux::ui::Frame &frame)
    {
        ++draws;
#if !defined(__LUX_PARSE_TIME__)
        lux::editor::ui::generated::consumer_scene_readersBindings().front().draw(session, target, frame);
#endif
    }
}
extern "C" __declspec(dllexport) void openSceneReaderPlugin(PluginApi *output)
{
    namespace ecs = lux::simulation::ecs;
    PluginApi result;
    result.schema = ecs::makeComponentSchema<consumer::RichComponent>(
        ecs::componentSchemaId("consumer.RichComponent"), 3, ecs::EComponentSnapshotPolicy::COPY, {}, nullptr,
        ecs::EComponentSemanticKind::DOMAIN_CONTRACT, true);
#if !defined(__LUX_PARSE_TIME__)
    result.reader = lux::editor::ui::generated::consumer_scene_readersBindings().front();
#endif
    result.reader.draw = &draw;
    result.populate = &populate;
    result.verify = &verify;
    result.drawCalls = [] { return draws; };
    *output = std::move(result);
}
