#include <lux/engine/ui/UI.hpp>

#include <array>
#include <memory>

namespace {
class DemoPane final : public lux::object::Object<DemoPane, lux::ui::Pane> {
public:
  DemoPane(lux::object::ObjectDispatcherRef dispatcher, lux::ui::PaneId id)
      : Object(dispatcher, std::move(id), lux::ui::PaneTypeId{"demo.inspector"},
               "UI vNext Demo") {}

  [[nodiscard]] std::span<const lux::ui::UiContextIdView>
  contexts() const noexcept override {
    return contexts_;
  }

  void save() noexcept { ++save_count_; }

protected:
  void draw(lux::ui::PaneDrawContext &context) override {
    context.activateContext(lux::ui::UiContextIdView{"demo.local"});
    ImGui::TextUnformatted(
        "Pane + Context + Command + Factory + Layout + Viewport");
    static_cast<void>(viewport_.draw(ImTextureID{}));
  }

private:
  std::array<lux::ui::UiContextIdView, 1> contexts_{
      lux::ui::UiContextIdView{"demo.selection"}};
  lux::ui::ViewportElement viewport_;
  int save_count_{0};
};
} // namespace

int main() {
  lux::ui::UISession session;
  auto factory = session.registerFactory(lux::ui::PaneFactory{
      lux::ui::PaneTypeId{"demo.inspector"}, "Demo Inspector",
      [](lux::object::ObjectDispatcherRef dispatcher,
         lux::ui::PaneId id) -> std::unique_ptr<lux::ui::Pane> {
        return std::make_unique<DemoPane>(std::move(dispatcher), std::move(id));
      }});
  if (!factory)
    return 1;

  auto first = session.createPane(lux::ui::PaneTypeIdView{"demo.inspector"},
                                  lux::ui::PaneId{"demo.first"});
  auto second = session.createPane(lux::ui::PaneTypeIdView{"demo.inspector"},
                                   lux::ui::PaneId{"demo.second"});
  if (!first || !second)
    return 2;
  auto first_registration = session.registerPane(*first);
  auto second_registration = session.registerPane(*second);
  if (!first_registration || !second_registration)
    return 3;

  auto save_command = session.commandRouter().defineCommand(
      {lux::ui::UiCommandId{"save"}, "Save"});
  if (!save_command)
    return 4;
  auto save = session.commandRouter().bind<&DemoPane::save>(
      *save_command, lux::ui::UiContextId{"demo.selection"}, *first,
      static_cast<DemoPane &>(*first));
  if (!save)
    return 5;

  for (int frame = 0; frame < 3; ++frame) {
    session.beginFrame({640.0F, 360.0F}, 1.0F / 60.0F);
    session.drawPanes();
    static_cast<void>(session.endFrame());
  }
  return session.captureLayout().bytes().empty() ? 6 : 0;
}
