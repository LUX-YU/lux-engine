#include <lux/engine/editor/widgets/GraphCanvas.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <cassert>
#include <cstdio>
#include <limits>
#include <cmath>
#include <chrono>
#include <algorithm>

namespace lux::editor::widgets
{
    // Test-only access to the owned public node-editor API; no backend implementation headers.
    struct GraphCanvasTestAccess
    {
        static void seedNavigation(GraphCanvas& view)
        {
            view.canvas_.reset();
            view.saved_view_ = R"({"view":{"scroll":{"x":7,"y":11},"zoom":1.25}})";
            view.canvas_ = view.makeCanvas();
        }
        static std::pair<float, ImVec2> navigation(GraphCanvas& view)
        {
            CanvasScope scope(view.canvas_.get());
            return {ax::NodeEditor::GetCurrentZoom(), ax::NodeEditor::ScreenToCanvas({0, 0})};
        }
        static ui::Point nodePoint(GraphCanvas& view, std::uint64_t id)
        {
            CanvasScope scope(view.canvas_.get());
            auto point = ax::NodeEditor::GetNodePosition(view.ids_.node(id));
            point.x += 10;
            point.y += 10;
            point = ax::NodeEditor::CanvasToScreen(point);
            return {point.x, point.y};
        }
        static bool selected(GraphCanvas& view, std::uint64_t id)
        {
            CanvasScope scope(view.canvas_.get());
            ax::NodeEditor::NodeId node;
            return ax::NodeEditor::GetSelectedNodes(&node, 1) == 1 && view.ids_.source(node) == id;
        }
    };
}

using namespace lux;
using namespace lux::editor::widgets;
namespace
{
    template <class T> auto take(T value)
    {
        assert(value);
        return std::move(*value);
    }
    struct Window final : ui::Pane
    {
        ui::Layout layout;
        GraphCanvas canvas;
        explicit Window(object::ObjectDispatcherRef dispatcher)
            : Pane(dispatcher, ui::PaneId{"churn"}, ui::PaneTypeId{"churn"}, "Graph churn"),
              layout(*this, ui::ElementId{"layout"}), canvas(layout, ui::ElementId{"canvas"})
        {
            assert(setContent(layout));
        }
    };
}
int main()
{
    NodeCanvasIds ids;
    const auto old = ids.node(UINT64_MAX);
    const auto pin = ids.pin(UINT64_MAX);
    assert(old.Get() != pin.Get() && ids.source(old) == UINT64_MAX);
    ids.retire();
    const auto replacement = ids.node(UINT64_MAX);
    assert(ids.source(old) == 0 && ids.source(pin) == 0 && replacement.Get() != old.Get());
    auto messages = take(object::ObjectMessageQueue::create(64));
    auto root = take(ui::Root::create(messages.dispatcherRef(), {.docking = false}));
    Window view(messages.dispatcherRef());
    GraphCanvasTestAccess::seedNavigation(view.canvas);
    auto attach = take(root->prepareMount(view));
    assert(root->commit(attach));
    ui::DrawData draw;
    std::uint64_t previous{};
    unsigned compactions{};
    unsigned selection_events{};
    auto selection = take(object::LuxObject::connect(
        &view.canvas,
        &GraphCanvas::selected,
        [&](std::span<const std::uint64_t> selected) noexcept {
            assert(selected.size() == 1 && selected.front() == 42);
            ++selection_events;
        }
    ));
    std::vector<double> samples;
    samples.reserve(9990);
    for (unsigned i{}; i != 10000; ++i)
    {
        const auto started = std::chrono::steady_clock::now();
        const auto identity = UINT64_MAX - 3 * std::uint64_t(i);
        std::vector<CanvasNode> nodes{
            {42, "persistent", {}, {37, 59}, true},
            {identity, "node", {{identity - 1, "pin", true}}, {137, 159}, true}
        };
        std::vector<CanvasLink> links;
        const auto before = view.canvas.retainedIds();
        const bool adopted = view.canvas.setGraph(std::move(nodes), std::move(links), false);
        if (!adopted)
        {
            const auto navigation = GraphCanvasTestAccess::navigation(view.canvas);
            // Pending tool input blocks compaction and preserves both the accepted display and candidate.
            assert(nodes.size() == 2 && view.canvas.nodes().back().id == previous);
            assert(view.canvas.retainedIds() == before);
            assert(root->feedInput(ui::PointerMove{{-1000, -1000}}));
            assert(root->feedInput(ui::PointerButton{ui::EPointerButton::RIGHT, true}));
            assert(root->update({{800, 600}, .016F}, &draw));
            assert(!view.canvas.setGraph(std::move(nodes), std::move(links), true));
            assert(nodes.size() == 2 && view.canvas.retainedIds() == before);
            assert(root->feedInput(ui::PointerButton{ui::EPointerButton::RIGHT, false}));
            assert(root->update({{800, 600}, .016F}, &draw));
            assert(view.canvas.setGraph(std::move(nodes), std::move(links), true));
            assert(root->update({{800, 600}, .016F}, &draw));
            const auto first_frame = GraphCanvasTestAccess::navigation(view.canvas);
            assert(std::abs(navigation.first - first_frame.first) < .001F);
            assert(std::abs(navigation.second.x - first_frame.second.x) < .001F);
            assert(std::abs(navigation.second.y - first_frame.second.y) < .001F);
            assert(root->update({{800, 600}, .016F}, &draw));
            const auto restored = GraphCanvasTestAccess::navigation(view.canvas);
            std::printf(
                "canvas compaction: zoom %.6f -> %.6f; origin (%.6f,%.6f) -> (%.6f,%.6f)\n",
                navigation.first,
                restored.first,
                navigation.second.x,
                navigation.second.y,
                restored.second.x,
                restored.second.y
            );
            std::fflush(stdout);
            assert(std::abs(navigation.first - restored.first) < .001F);
            assert(std::abs(navigation.second.x - restored.second.x) < .001F);
            assert(std::abs(navigation.second.y - restored.second.y) < .001F);
            assert(GraphCanvasTestAccess::selected(view.canvas, 42));
            ++compactions;
        }
        assert(view.canvas.nodes().front().position == ui::Point(37, 59));
        assert(root->update({{800, 600}, .016F}, &draw));
        if (i == 0)
        {
            assert(root->update({{800, 600}, .016F}, &draw));
            assert(root->feedInput(ui::PointerMove{GraphCanvasTestAccess::nodePoint(view.canvas, 42)}));
            assert(root->update({{800, 600}, .016F}, &draw));
            assert(root->feedInput(ui::PointerButton{ui::EPointerButton::LEFT, true}));
            assert(root->update({{800, 600}, .016F}, &draw));
            assert(root->feedInput(ui::PointerButton{ui::EPointerButton::LEFT, false}));
            assert(root->update({{800, 600}, .016F}, &draw));
            const auto point = GraphCanvasTestAccess::nodePoint(view.canvas, 42);
            std::fprintf(stderr, "canvas click point=(%g,%g) events=%u selected=%d\n", point.x, point.y,
                selection_events, GraphCanvasTestAccess::selected(view.canvas, 42));
            assert(selection_events == 1);
            assert(GraphCanvasTestAccess::selected(view.canvas, 42));
        }
        assert(view.canvas.retainedIds() <= 4096);
        previous = identity;
        if (i >= 10)
            samples.push_back(
                std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count()
            );
    }
    assert(compactions >= 2);
    auto detach = take(root->prepareDetach(view));
    assert(root->commit(detach));
    std::printf(
        "XQ22 actual GraphCanvas 10000 replacements, large IDs, bounded maps, pending-input deferral: %u compactions\n",
        compactions
    );
    std::ranges::sort(samples);
    std::printf(
        "BQ3 actual setGraph plus Root layout/draw: warmup=10 samples=%zu p50_us=%.3f p95_us=%.3f "
        "p99_us=%.3f max_us=%.3f; retained identities <=4096\n",
        samples.size(),
        samples[samples.size() / 2],
        samples[samples.size() * 95 / 100],
        samples[samples.size() * 99 / 100],
        samples.back()
    );
}
