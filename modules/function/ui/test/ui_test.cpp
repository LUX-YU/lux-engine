#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include <lux/engine/ui/UI.hpp>
#include <lux/engine/ui/detail/DragDropEncoding.hpp>
#include <lux/engine/ui/detail/UISessionDiagnostics.hpp>

#include <array>
#include <concepts>
#include <memory>
#include <type_traits>
#include <vector>

static_assert(
    std::same_as<decltype(lux::ui::UiPointerButton::button), ImGuiMouseButton>);
static_assert(std::same_as<decltype(lux::ui::UiKey::key), ImGuiKey>);

namespace {
class TestPane final : public lux::object::Object<TestPane, lux::ui::Pane> {
public:
  TestPane(lux::object::ObjectDispatcherRef dispatcher, lux::ui::PaneId id,
           std::string title)
      : Object(dispatcher, std::move(id), lux::ui::PaneTypeId{"test.pane"},
               std::move(title)) {}

  [[nodiscard]] std::span<const lux::ui::UiContextIdView>
  contexts() const noexcept override {
    return contexts_;
  }

  void contextualDelete() noexcept { ++contextual_count; }
  void globalDelete() noexcept { ++global_count; }
  [[nodiscard]] bool contextualEnabled() const noexcept { return false; }

  int contextual_count{0};
  int global_count{0};

protected:
  void draw(lux::ui::PaneDrawContext &context) override {
    context.activateContext(lux::ui::UiContextIdView{"test.local.first"});
    context.activateContext(lux::ui::UiContextIdView{"test.local.second"});
    ImGui::TextUnformatted("UI vNext");
  }

private:
  std::array<lux::ui::UiContextIdView, 1> contexts_{
      lux::ui::UiContextIdView{"test.selection"}};
};

class Receiver final : public lux::object::Object<Receiver> {
public:
  void invoke() noexcept { ++count; }
  int count{0};
};
} // namespace

int main() {
  std::array<std::byte, lux::ui::detail::kInlineDragDropBytes> inline_payload{};
  std::vector<std::byte> heap_payload;
  const std::array small_content{std::byte{0x1}, std::byte{0x2},
                                 std::byte{0x3}};
  const auto small_encoded = lux::ui::detail::encodeDragDropPayload(
      lux::ui::PayloadTypeIdView{"test.payload"}, small_content, inline_payload,
      heap_payload);
  assert(heap_payload.empty());
  const auto small_decoded =
      lux::ui::detail::decodeDragDropPayload(small_encoded);
  assert(small_decoded);
  assert(small_decoded->type == lux::ui::PayloadTypeIdView{"test.payload"});
  assert(std::ranges::equal(small_decoded->bytes, small_content));

  std::vector<std::byte> large_content(512, std::byte{0x7});
  const auto large_encoded = lux::ui::detail::encodeDragDropPayload(
      lux::ui::PayloadTypeIdView{"test.large_payload"}, large_content,
      inline_payload, heap_payload);
  assert(!heap_payload.empty());
  const auto large_decoded =
      lux::ui::detail::decodeDragDropPayload(large_encoded);
  assert(large_decoded);
  assert(large_decoded->bytes.size() == large_content.size());

  auto *external_context = ImGui::CreateContext();
  ImGui::SetCurrentContext(external_context);
  {
    lux::ui::UISession first_session;
    lux::ui::UISession second_session;
    assert(lux::ui::detail::UISessionDiagnosticsAccess::contextIdentity(
               first_session) !=
           lux::ui::detail::UISessionDiagnosticsAccess::contextIdentity(
               second_session));
    assert(ImGui::GetCurrentContext() == external_context);
    first_session.beginFrame({64.0F, 64.0F}, 1.0F / 60.0F);
    static_cast<void>(first_session.endFrame());
    assert(ImGui::GetCurrentContext() == external_context);
    second_session.beginFrame({64.0F, 64.0F}, 1.0F / 60.0F);
    static_cast<void>(second_session.endFrame());
    assert(ImGui::GetCurrentContext() == external_context);
  }
  {
    lux::ui::UISession session;
    assert(ImGui::GetCurrentContext() == external_context);
    TestPane first{session.dispatcherRef(), lux::ui::PaneId{"first"}, "First"};
    TestPane duplicate{session.dispatcherRef(), lux::ui::PaneId{"first"},
                       "Duplicate"};
    TestPane second{session.dispatcherRef(), lux::ui::PaneId{"second"},
                    "Second"};

    auto first_registration = session.registerPane(first);
    assert(first_registration);
    auto duplicate_registration = session.registerPane(duplicate);
    assert(!duplicate_registration);
    auto second_registration = session.registerPane(second);
    assert(second_registration);

    lux::ui::UISession foreign_session;
    TestPane foreign{foreign_session.dispatcherRef(),
                     lux::ui::PaneId{"foreign"}, "Foreign"};
    auto foreign_registration = session.registerPane(foreign);
    assert(!foreign_registration);
    assert(foreign_registration.error() ==
           lux::ui::EUiRegistrationError::FOREIGN_SESSION);

    auto factory_registration = session.registerFactory(lux::ui::PaneFactory{
        lux::ui::PaneTypeId{"test.pane"}, "Test Pane",
        [](lux::object::ObjectDispatcherRef dispatcher,
           lux::ui::PaneId id) -> std::unique_ptr<lux::ui::Pane> {
          return std::make_unique<TestPane>(std::move(dispatcher),
                                            std::move(id), "Created");
        }});
    assert(factory_registration);
    auto duplicate_factory = session.registerFactory(lux::ui::PaneFactory{
        lux::ui::PaneTypeId{"test.pane"}, "Duplicate",
        [](lux::object::ObjectDispatcherRef,
           lux::ui::PaneId) -> std::unique_ptr<lux::ui::Pane> { return {}; }});
    assert(!duplicate_factory);
    auto created = session.createPane(lux::ui::PaneTypeIdView{"test.pane"},
                                      lux::ui::PaneId{"created"});
    assert(created);

    int focus_changes = 0;
    bool last_focus = false;
    auto focus_connection = first.observeScoped<lux::ui::Pane::focusChanged>(
        [&](const lux::ui::PaneFocusChanged &change) noexcept {
          ++focus_changes;
          last_focus = change.focused;
        });
    static_cast<void>(focus_connection);

    int visibility_changes = 0;
    auto visibility_connection =
        first.observeScoped<lux::ui::Pane::visibilityChanged>(
            [&](const lux::ui::PaneVisibilityChanged &) noexcept {
              ++visibility_changes;
            });
    static_cast<void>(visibility_connection);
    first.setVisible(false);
    first.setVisible(true);
    assert(visibility_changes == 2);

    auto &router = session.commandRouter();
    auto delete_command =
        router.defineCommand({lux::ui::UiCommandId{"delete"}, "Delete"});
    assert(delete_command);
    auto duplicate_command =
        router.defineCommand({lux::ui::UiCommandId{"delete"}, "Delete Again"});
    assert(!duplicate_command);
    auto contextual =
        router.bind<&TestPane::contextualDelete, &TestPane::contextualEnabled>(
            *delete_command, lux::ui::UiContextId{"test.selection"}, first,
            first);
    assert(contextual);
    auto duplicate_binding = router.bind<&TestPane::contextualDelete>(
        *delete_command, lux::ui::UiContextId{"test.selection"}, first, first);
    assert(!duplicate_binding);
    auto second_contextual = router.bind<&TestPane::contextualDelete>(
        *delete_command, lux::ui::UiContextId{"test.selection"}, second,
        second);
    assert(second_contextual);
    auto global =
        router.bindGlobal<&TestPane::globalDelete>(*delete_command, first);
    assert(global);

    session.beginFrame({800.0F, 600.0F}, 1.0F / 60.0F);
    session.drawPanes();
    static_cast<void>(session.endFrame());
    assert(session.requestFocus(first.id().view()));
    assert(!first.focused());
    session.beginFrame({800.0F, 600.0F}, 1.0F / 60.0F);
    session.drawPanes();
    static_cast<void>(session.endFrame());
    if (!first.focused()) {
      session.beginFrame({800.0F, 600.0F}, 1.0F / 60.0F);
      session.drawPanes();
      static_cast<void>(session.endFrame());
    }
    assert(first.focused());
    assert(focus_changes == 1 && last_focus);
    assert(session.focusedContexts().size() == 4);
    assert(session.focusedContexts()[0] ==
           lux::ui::UiContextIdView{"test.local.second"});
    assert(session.focusedContexts()[2] ==
           lux::ui::UiContextIdView{"test.selection"});
    assert(router.state(*delete_command).found);
    assert(!router.state(*delete_command).enabled);
    assert(router.invoke(*delete_command) ==
           lux::ui::ECommandDispatchResult::DISABLED);
    assert(first.contextual_count == 0);
    assert(first.global_count == 0);
    contextual->reset();
    assert(router.invoke(*delete_command) ==
           lux::ui::ECommandDispatchResult::EXECUTED);
    assert(first.global_count == 1);

    assert(session.requestFocus(second.id().view()));
    assert(first.focused());
    session.beginFrame({800.0F, 600.0F}, 1.0F / 60.0F);
    session.drawPanes();
    static_cast<void>(session.endFrame());
    if (first.focused()) {
      session.beginFrame({800.0F, 600.0F}, 1.0F / 60.0F);
      session.drawPanes();
      static_cast<void>(session.endFrame());
    }
    assert(!first.focused());
    assert(focus_changes == 2 && !last_focus);
    assert(router.invoke(*delete_command) ==
           lux::ui::ECommandDispatchResult::EXECUTED);
    assert(second.contextual_count == 1);
    assert(first.global_count == 1);

    Receiver fallback_global_receiver;
    auto fallback_command =
        router.defineCommand({lux::ui::UiCommandId{"fallback"}, "Fallback"});
    assert(fallback_command);
    auto fallback_global = router.bindGlobal<&Receiver::invoke>(
        *fallback_command, fallback_global_receiver);
    assert(fallback_global);
    lux::ui::CommandRegistration expired_contextual;
    {
      Receiver contextual_receiver;
      auto contextual_receiver_binding = router.bind<&Receiver::invoke>(
          *fallback_command, lux::ui::UiContextId{"test.selection"}, second,
          contextual_receiver);
      assert(contextual_receiver_binding);
      expired_contextual = std::move(*contextual_receiver_binding);
      assert(router.invoke(*fallback_command) ==
             lux::ui::ECommandDispatchResult::EXECUTED);
      assert(contextual_receiver.count == 1);
      assert(fallback_global_receiver.count == 0);
    }
    assert(router.invoke(*fallback_command) ==
           lux::ui::ECommandDispatchResult::EXECUTED);
    assert(fallback_global_receiver.count == 1);

    lux::ui::CommandRegistration dead_binding;
    {
      Receiver receiver;
      const auto temporary = router.defineCommand(
          {lux::ui::UiCommandId{"temporary"}, "Temporary"});
      assert(temporary);
      auto binding = router.bindGlobal<&Receiver::invoke>(*temporary, receiver);
      assert(binding);
      dead_binding = std::move(*binding);
    }
    const auto temporary =
        router.findCommand(lux::ui::UiCommandIdView{"temporary"});
    assert(temporary);
    assert(router.invoke(*temporary) ==
           lux::ui::ECommandDispatchResult::NOT_FOUND);

    session.beginFrame({800.0F, 600.0F}, 1.0F / 60.0F);
    session.drawPanes();
    assert(session.endFrame() != nullptr);
    const auto layout = session.captureLayout();
    assert(!layout.bytes().empty());
    auto decoded = lux::ui::LayoutSnapshot::fromBytes(layout.bytes());
    assert(decoded);
    assert(session.restoreLayout(*decoded));
    session.feedInput(lux::ui::UiPointerMove{20.0F, 30.0F});
    session.feedInput(lux::ui::UiKey{ImGuiKey_Delete, true});
    session.feedInput(lux::ui::UiPointerButton{ImGuiMouseButton_Left, true});
    session.beginFrame({800.0F, 600.0F}, 1.0F / 60.0F);
    session.drawPanes();
    static_cast<void>(session.endFrame());
    assert(ImGui::GetCurrentContext() == external_context);

    lux::ui::MenuModel menu{{lux::ui::MenuItem{
        lux::ui::EMenuItemKind::COMMAND, "Delete", *delete_command, {}}}};
    lux::ui::ToolbarModel toolbar{{lux::ui::ToolbarItem{
        lux::ui::EToolbarItemKind::COMMAND, *delete_command}}};
    assert(menu.items.front().command == toolbar.items.front().command);

    second_registration->reset();
    auto replacement_registration = session.registerPane(duplicate);
    assert(!replacement_registration);
    first_registration->reset();
    replacement_registration = session.registerPane(duplicate);
    assert(replacement_registration);
  }
  assert(ImGui::GetCurrentContext() == external_context);

  lux::ui::CommandHandle stale_handle;
  {
    lux::ui::CommandRouter retired_router;
    auto command = retired_router.defineCommand(
        {lux::ui::UiCommandId{"stale.command"}, "Stale"});
    assert(command);
    stale_handle = *command;
  }
  lux::ui::CommandRouter replacement_router;
  auto replacement_command = replacement_router.defineCommand(
      {lux::ui::UiCommandId{"stale.command"}, "Replacement"});
  assert(replacement_command);
  assert(!replacement_router.state(stale_handle).found);
  assert(replacement_router.command(stale_handle) == nullptr);

  ImGui::SetCurrentContext(external_context);
  ImGui::DestroyContext(external_context);
}
