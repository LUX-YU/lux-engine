#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Root.hpp>

#ifndef LUX_CALLBACK_CASE
#define LUX_CALLBACK_CASE 0
#endif

// The same public headers and actual factory calls qualify both acceptance and rejection.
void callbackContract(lux::editor::EditorContext& context, lux::ui::Root& root)
{
    using namespace lux;
    auto assembly = [](editor::EditorContext&) noexcept(LUX_CALLBACK_CASE != 1) -> editor::FrameworkResult<void>
    { return {}; };
    editor::EditorContext::Assembly borrowed(assembly);
    static_cast<void>(borrowed);
    auto ui = [](editor::EditorContext&, const editor::PaneDescription&) noexcept(
                  LUX_CALLBACK_CASE != 2
              ) -> editor::FrameworkResult<std::unique_ptr<ui::Pane>> { return std::make_unique<ui::Pane>("Test"); };
    static_cast<void>(context.ui().registerFactory("callback.test", std::move(ui)));
    static_cast<void>(context.services().registerFactory<int>(
        [](editor::EditorContext&) noexcept(LUX_CALLBACK_CASE != 3) -> editor::FrameworkResult<std::unique_ptr<int>>
        { return std::make_unique<int>(1); }
    ));
    static_cast<void>(context.sceneTools().registerFactory<int>(
        [](const world::WorldDescription&) noexcept { return true; },
        [](editor::EditorContext&, const world::WorldDescription&) noexcept(LUX_CALLBACK_CASE != 4)
            -> editor::FrameworkResult<std::unique_ptr<int>> { return std::make_unique<int>(2); }
    ));
    auto capture = [](const ui::DrawData&) noexcept(LUX_CALLBACK_CASE != 5) -> cxx::expected<void, ui::ECaptureError>
    { return {}; };
    ui::Root::Capture captured(capture);
    static_cast<void>(captured);
    auto visit = [](ui::Pane&) noexcept(LUX_CALLBACK_CASE != 6) {};
    static_cast<void>(root.forEachPane(visit));
}
