#include <lux/engine/editor/ui/scene/ComponentReadBinding.hpp>
#include <transform_inspectors.inspector.generated.hpp>
#include <hierarchy_inspectors.inspector.generated.hpp>
#include <visual_inspectors.inspector.generated.hpp>
namespace lux::editor::ui
{
    std::vector<ComponentReadBinding> firstPartySceneReaders()
    {
        std::vector<ComponentReadBinding> result;
        const auto append = [&](auto readers) { result.insert(result.end(), readers.begin(), readers.end()); };
        append(generated::transform_inspectorsBindings());
        append(generated::hierarchy_inspectorsBindings());
        append(generated::visual_inspectorsBindings());
        return result;
    }
} // namespace lux::editor::ui
