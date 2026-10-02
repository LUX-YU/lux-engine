#include <lux/engine/editor/extensions/EditorExtension.hpp>
#include <lux/engine/ui/Controls.hpp>
#include "SampleEditorExport.hpp"

namespace
{
    class SamplePane final : public lux::ui::Pane
    {
    public:
        explicit SamplePane(const lux::editor::views::ViewFactoryInput& input)
            : Pane(input.dispatcher(), input.paneId(), lux::ui::PaneTypeId{"sample.editor"}, "Plugin editor"),
              text_(*this, lux::ui::ElementId{"message"}, "External V8 editor extension")
        {
            setContent(text_);
        }

    private:
        lux::ui::Label text_;
    };
}

extern "C" SAMPLE_EXTERNAL_EDITOR_EXPORT const lux::editor::extensions::EditorExtensionExports* lux_editor_exports_v8(
) noexcept
{
    using namespace lux::editor;
    static const extensions::EditorExtensionExports exports{
        .counts = {.views = 1},
        .contribute = +[](extensions::ContributionDraft& draft,
                          contracts::CodeLease code) -> extensions::ContributionResult<void> {
            draft.views.push_back(std::make_shared<views::ViewFactoryEntry>(
                code,
                views::ViewFactoryDescriptor{
                    views::ViewTypeId{"sample.editor"},
                    "External editor",
                    lux::cxx::typeToken<std::monostate>()
                },
                [code](const views::ViewFactoryInput& input) -> views::ViewFactoryResult<views::DetachedView> {
                    return views::DetachedView{code, std::make_unique<SamplePane>(input)};
                }
            ));
            return {};
        }
    };
    return &exports;
}
