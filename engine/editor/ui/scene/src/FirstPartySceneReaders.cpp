#include <lux/engine/editor/ui/scene/ComponentReadBinding.hpp>
#include <lux/engine/simulation/ecs/Transform.scene_reader.hpp>
#include <lux/engine/simulation/ecs/Parent.scene_reader.hpp>
#include <lux/engine/simulation/ecs/Visual.scene_reader.hpp>
namespace lux::editor::ui
{
    std::vector<ComponentReadBinding> firstPartySceneReaders()
    {
        std::vector<ComponentReadBinding> result;
        const auto append = [&](auto readers) { result.insert(result.end(), readers.begin(), readers.end()); };
        append(generated::transformSceneReaders());
        append(generated::hierarchySceneReaders());
        append(generated::visualSceneReaders());
        return result;
    }
} // namespace lux::editor::ui
