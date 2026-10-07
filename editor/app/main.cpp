#include <cstdio>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/LuxEngine.hpp>
#include <lux/engine/error/ErrorRegistry.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/ui/Pane.hpp>

namespace
{
    class WelcomePane final : public lux::ui::Pane
    {
    public:
        WelcomePane(lux::editor::EditorContext& context, const lux::editor::PaneDescription& description)
            : Pane(description.title), layout_{}, label_("LuxEngine Editor Framework v2"),
              detail_("Framework ready. Authoring tools are not loaded.")
        {
            if (!layout_.addElement(label_) || !layout_.addElement(detail_) || !addElement(layout_))
            {
                std::terminate();
            }
        }

    private:
        lux::ui::Layout layout_;
        lux::ui::Label label_, detail_;
    };
} // namespace
int main()
{
    auto engine = lux::editor::LuxEngine::create();
    if (!engine)
    {
        std::fprintf(stderr, "%s\n", lux::error::format(engine.error()).c_str());
        return 1;
    }
    auto assemble = [](lux::editor::EditorContext& context) noexcept -> lux::editor::FrameworkResult<void>
    {
        return context.ui().registerFactory(
            "framework.welcome",
            [](lux::editor::EditorContext& value, const lux::editor::PaneDescription& description
            ) noexcept -> lux::editor::FrameworkResult<std::unique_ptr<lux::ui::Pane>>
            { return std::unique_ptr<lux::ui::Pane>{new WelcomePane(value, description)}; }
        );
    };
    std::error_code error;
    auto root = std::filesystem::current_path(error);
    if (error)
    {
        return 1;
    }
    const lux::editor::EditorLayout layout{{"framework.welcome", "welcome", "Welcome"}};
    auto opened = (*engine)->openProject({"Framework", std::move(root)}, layout, assemble);
    if (!opened)
    {
        std::fprintf(stderr, "%s\n", lux::error::format(opened.error()).c_str());
        return 2;
    }
    auto result = (*engine)->exec();
    if (!result)
    {
        std::fprintf(stderr, "%s\n", lux::error::format(result.error()).c_str());
    }
    return result ? 0 : 3;
}
