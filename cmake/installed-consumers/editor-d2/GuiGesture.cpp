#include <InspectorWidget.hpp>
#include <cassert>
#include <consumer/Component.hpp>
#include <cstdio>

namespace consumer
{
    void checkClippedGesture(lux::editor::scene::SceneEditor &document, lux::world::WorldObjectId object,
                             lux::ui::Frame &frame)
    {
        using namespace lux::editor;
        // A separate ImGui context injects IO events; this is not physical desktop evidence.
        struct Context final
        {
            ImGuiContext *previous{ImGui::GetCurrentContext()};
            ImGuiContext *current{ImGui::CreateContext()};
            ~Context()
            {
                ImGui::DestroyContext(current);
                ImGui::SetCurrentContext(previous);
            }
        } context;
        ImGui::SetCurrentContext(context.current);
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = {640, 480};
        io.DeltaTime = 1.0F / 60;
        io.MouseDoubleClickTime = 0; // Each scripted press is a separate drag, not a text-entry double click.
        unsigned char *pixels{};
        int width{}, height{};
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        gui::InspectorInteraction interaction(document, "clipped-gesture");
        const auto before = document.historyView()->history;
        const auto access = [](auto &value) noexcept { return &value.settings.gain; };
        const auto read = [&]()
        {
            return static_cast<const Component *>(document.component(object, lux::cxx::typeToken<Component>()))
                ->settings.gain;
        };
        const auto original = read();
        ImVec2 center{};
        const auto draw = [&](bool visible)
        {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos({0, 0});
            ImGui::SetNextWindowSize({600, 400});
            ImGui::Begin("Gesture regression");
            {
                auto table = frame.table({lux::ui::WidgetIdView{"fields"}, 2});
                assert(table.visible());
                if (visible)
                {
                    interaction.field<Component, double>(
                        document, object, frame, "settings.gain", "Gain", access,
                        [&](double &value, auto &)
                        {
                            const bool changed = ImGui::DragScalar("##value", ImGuiDataType_Double, &value, 0.1F);
                            const auto minimum = ImGui::GetItemRectMin();
                            const auto maximum = ImGui::GetItemRectMax();
                            center = {(minimum.x + maximum.x) / 2, (minimum.y + maximum.y) / 2};
                            return gui::generated_support::edited(changed);
                        },
                        false);
                }
            }
            assert(interaction.finishDraw());
            ImGui::End();
            ImGui::Render();
        };
        draw(true);
        draw(true);
        io.AddMousePosEvent(center.x, center.y);
        draw(true);
        io.AddMouseButtonEvent(0, true);
        draw(true);
        assert(interaction.active());
        io.AddMousePosEvent(center.x + 40, center.y);
        draw(true);
        assert(read() != original && document.historyView()->history.current == before.current);
        // Pagination/clipping omits the active field while release arrives through the same IO system.
        io.AddMouseButtonEvent(0, false);
        draw(false);
        draw(false);
        draw(false);
        std::printf("Clipped gesture: active=%d cursor=%zu before=%zu original=%.3f value=%.3f\n", interaction.active(),
                    document.historyView()->history.cursor, before.cursor, original, read());
        std::fflush(stdout);
        assert(!interaction.active());
        assert(document.historyView()->history.cursor == before.cursor + 1);
        draw(true);
        assert(document.historyView()->history.cursor == before.cursor + 1);
        assert(document.undo() && read() == original);
        assert(document.historyView()->history.current == before.current);

        const auto no_change = document.historyView()->history;
        io.AddMousePosEvent(center.x, center.y);
        draw(true);
        io.AddMouseButtonEvent(0, true);
        draw(true);
        assert(interaction.active());
        io.AddMouseButtonEvent(0, false);
        draw(true);
        assert(!interaction.active() && read() == original);
        assert(document.historyView()->history.current == no_change.current);
        assert(document.historyView()->history.revision == no_change.revision);

        io.AddMouseButtonEvent(0, true);
        draw(true);
        io.AddMousePosEvent(center.x + 60, center.y);
        draw(true);
        assert(interaction.active() && read() != original);
        draw(false);
        assert(!interaction.active() && document.historyView()->history.cursor == before.cursor + 1);
        // Reappear while still held: the old widget must not restart its retired preview.
        draw(true);
        io.AddMouseButtonEvent(0, false);
        draw(true);
        draw(true);
        assert(!interaction.active() && document.historyView()->history.cursor == before.cursor + 1);
        assert(document.undo() && read() == original);
        std::puts("PASS injected ImGui gesture: omitted control commits once while held or released; redraw does not "
                  "restart; unchanged click preserves revision; Undo restores");
    }
} // namespace consumer
