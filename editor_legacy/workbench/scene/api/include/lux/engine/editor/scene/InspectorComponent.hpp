#pragma once
#include <lux/engine/editor/scene/SceneEditError.hpp>
#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/engine/ui/Ids.hpp>
#include <memory>
#include <string>
namespace lux::ui
{
    class Element;
}
namespace lux::editor::scene
{
    class InspectorFields;
    struct InspectorComponent final
    {
        using CreateResult = SceneEditResult<std::unique_ptr<lux::ui::Element>>;
        cxx::TypeToken type;
        std::string label;
        CreateResult (*create)(lux::ui::Element&, lux::ui::ElementId, InspectorFields&){};
        std::shared_ptr<const void> code;
    };
}
