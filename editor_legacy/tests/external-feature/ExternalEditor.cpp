#include <exception>
#include <lux/engine/editor/extensions/EditorExtension.hpp>
#include <lux/engine/ui/Controls.hpp>
#include "SampleEditorExport.hpp"

namespace
{
    class SamplePane final : public lux::ui::Pane
    {
    public:
        explicit SamplePane(const lux::editor::desktop::UiCreateInfo& input)
            : Pane(input.dispatcher, input.instance, lux::ui::PaneTypeId{"sample.editor"}, "Plugin editor"),
              text_(*this, lux::ui::ElementId{"message"}, "External V10 editor extension")
        {
            if (!setContent(text_))
                std::terminate(); // Fixed content in a detached Pane.
        }

    private:
        lux::ui::Label text_;
    };

    constexpr lux::editor::desktop::UiDescriptor kSampleView{
        .type = lux::editor::views::ViewTypeIdView{"sample.editor"},
        .label = "External editor",
        .create = +[](lux::services::ServiceResolver&, const lux::editor::desktop::UiCreateInfo& input)
            -> lux::editor::desktop::UiResult<std::unique_ptr<lux::ui::Pane>>
        {
            return std::make_unique<SamplePane>(input);
        }
    };
} // namespace

extern "C" SAMPLE_EXTERNAL_EDITOR_EXPORT const lux::editor::extensions::EditorExtensionExports* lux_editor_exports_v10(
) noexcept
{
    using namespace lux::editor;
    static const extensions::EditorExtensionExports exports{
        .counts = {.ui = 1},
        .contribute =
            +[](extensions::ContributionDraft& draft, lux::object::CodeLease code) -> extensions::ContributionResult<void>
        {
            draft.ui.push_back(desktop::UiEntry::bind<kSampleView>(std::move(code)));
            return {};
        }
    };
    return &exports;
}
