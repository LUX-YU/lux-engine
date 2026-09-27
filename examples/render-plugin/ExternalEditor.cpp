#include <lux/engine/editor/metadata/EditorPluginExports.hpp>
#include <lux/engine/editor/CloseRequest.hpp>
#include <lux/engine/editor/PaneManager.hpp>
#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Root.hpp>
#include "SampleEditorExport.hpp"

namespace
{
    class SamplePane final : public lux::ui::Pane
    {
    public:
        explicit SamplePane(lux::ui::Root& root)
            : Pane(
                  root,
                  lux::ui::PaneId{"sample.editor/instance"},
                  lux::ui::PaneTypeId{"sample.editor"},
                  "Plugin editor"
              )
        {}

    private:
        void event(lux::object::EventView& event) noexcept override
        {
            using namespace lux::editor;
            if (const auto* request = event.getIf<CloseRequest>())
            {
                event.accept();
                if (request->action == ECloseAction::CANCEL)
                    return;
                CloseDecision decision{request->id, this, ECloseDecision::READY};
                static_cast<void>(lux::object::routeEvent(*this, root(), decision));
            }
        }
    };
}

extern "C" SAMPLE_EXTERNAL_EDITOR_EXPORT const lux::editor::EditorPluginExports* lux_editor_exports_v6() noexcept
{
    static const auto registration = [] {
        lux::editor::PaneRegistration result;
        result.type = lux::ui::PaneTypeId{"sample.editor"};
        result.name = "External editor";
        result.create = [](lux::editor::PaneManager& panes) noexcept -> lux::editor::PaneRegistration::CreateResult {
            if (auto* existing = panes.findFirst(lux::ui::PaneTypeIdView{"sample.editor"}))
                return std::ref(*existing);
            return panes.adopt(std::make_unique<SamplePane>(panes.root()));
        };
        return result;
    }();
    static const lux::editor::EditorPluginExports exports{
        sizeof(exports),
        lux::editor::kEditorPluginInterfaceVersion,
        [](lux::meta::ReflectionRegistry&, lux::meta::qual_type_index_fix_list&) {},
        nullptr,
        0,
        nullptr,
        0,
        &registration,
        1
    };
    return &exports;
}
