#include "InspectorFixture.hpp"
#if !defined(__LUX_PARSE_TIME__)
#include <perf_inspectors.inspector.generated.hpp>
#endif
#include <InspectorWidget.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/ui/UISession.hpp>
#include <chrono>
#include <cstdio>
#include <cassert>
namespace
{
    namespace ui = lux::editor::ui;
    using Clock = std::chrono::steady_clock;
    class BenchPane final : public lux::object::Object<BenchPane, lux::ui::Pane>
    {
        inspector_fixture::SmallComponent value_;
        ui::InspectorInteraction interaction_;
        void manual()
        {
            // Independently authored baseline with identical fields, options, IDs and geometry.
            using namespace ui::generated_support;
            {
                FieldScope field{{"Enabled"}, false, "inspector_fixture::SmallComponent::enabled"};
                static_cast<void>(immediate(ImGui::Checkbox("##value", &value_.enabled)));
            }
            {
                FieldScope field{{"Number"}, false, "inspector_fixture::SmallComponent::number"};
                const auto original = value_.number;
                constexpr double step = 1;
                const auto edit = edited(ImGui::InputScalar("##value", ImGuiDataType_Double,
                    &value_.number, &step, nullptr, "%.17g"));
                if (edit.changed && !std::isfinite(value_.number)) value_.number = original;
            }
            {
                FieldScope field{{"Text"}, false, "inspector_fixture::SmallComponent::text"};
                static_cast<void>(edited(ImGui::InputText("##value", &value_.text)));
            }
            {
                FieldScope field{{"Values"}, false, "inspector_fixture::SmallComponent::values"};
                for (unsigned i = 0; i < value_.values.size(); ++i)
                {
                    IdScope id{static_cast<int>(i)};
                    static_cast<void>(edited(ImGui::DragScalar("##value", ImGuiDataType_S32,
                        &value_.values[i], 0.1F, nullptr, nullptr, nullptr)));
                    if (ImGui::SmallButton("Remove"))
                    {
                        value_.values.erase(value_.values.begin() + i);
                        return;
                    }
                }
                if (ImGui::SmallButton("Add")) value_.values.emplace_back();
            }
        }
    public:
        bool generated;
        std::uint64_t callback_ns{}, vertices{}, indices{}, checksum{};
        BenchPane(lux::object::ObjectDispatcherRef dispatcher, bool selected)
            : Object(dispatcher, lux::ui::PaneId{"benchmark"}, lux::ui::PaneTypeId{"test"}, "Inspector"),
              generated(selected) {}
        void draw(lux::ui::Frame &frame, lux::ui::PaneDrawContext &) override
        {
            auto table = frame.table({lux::ui::WidgetIdView{"fields"}, 2, false, false, false, 140});
            assert(table.visible());
            auto *list = ImGui::GetWindowDrawList();
            const auto before_vertices = list->VtxBuffer.Size, before_indices = list->IdxBuffer.Size;
            const auto start = Clock::now();
#if !defined(__LUX_PARSE_TIME__)
            if (generated)
                static_cast<void>(ui::generated::draw_inspector_fixture__SmallComponent_00ff3a8f(value_, interaction_));
            else manual();
#endif
            callback_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
            vertices += list->VtxBuffer.Size - before_vertices;
            indices += list->IdxBuffer.Size - before_indices;
            assert(!interaction_.error[0] && !value_.enabled && value_.number == 1 && value_.text == "abc");
            checksum += 17 + value_.values.at(0) * 3 + value_.values.at(1) * 7 + value_.text.size();
        }
    };
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    const bool generated = std::string_view{argv[1]} == "generated";
    assert(generated || std::string_view{argv[1]} == "manual");
    lux::meta::ReflectionRegistry::initRegistry();
    {
        lux::ui::UISession session;
        BenchPane pane(session.dispatcherRef(), generated);
        auto token = session.registerPane(pane);
        assert(token);
        session.setSplitLayout({"", "benchmark", "", ""});
        const auto frame = [&] {
            auto scope = session.beginFrame({{1000, 800}, 1.0F / 60, {1, 1}});
            scope.drawPanes(); scope.finish();
            assert(session.captureFrame());
        };
        for (unsigned i = 0; i < 100; ++i) frame();
        pane.callback_ns = pane.vertices = pane.indices = pane.checksum = 0;
        const auto start = Clock::now();
        for (unsigned i = 0; i < 10000; ++i) frame();
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
        std::printf("{\"mode\":\"%s\",\"frames\":10000,\"warmup\":100,\"callback_ns\":%llu,"
                    "\"frame_ns\":%llu,\"vertices\":%llu,\"indices\":%llu,\"checksum\":%llu}\n",
                    argv[1], pane.callback_ns, static_cast<unsigned long long>(elapsed),
                    pane.vertices, pane.indices, pane.checksum);
        token->reset();
    }
    lux::meta::ReflectionRegistry::destroyRegistry();
}
