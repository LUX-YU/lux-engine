#include "../src/HighlightRenderer.hpp"
#include <cassert>
#include <iostream>
using namespace lux::render;
using namespace lux::editor::views;
int main()
{
    HighlightRenderer renderer;
    HighlightRenderer::Key key{};
    key.selection = 12;
    int captured{}, encoded{};
    RenderSubmissionState::Observer observer;
    auto capture = [&]() -> RenderResult<RenderSubmissionState> {
        ++captured;
        if (captured == 1)
            return lux::cxx::unexpected(RendererFailure{ERendererError::NOT_READY});
        auto value = RenderSubmissionState::create();
        assert(value);
        observer = value->observe();
        return std::move(*value);
    };
    auto encode = [&](auto&) { ++encoded; };
    assert(!renderer.prepare(key, capture, encode));
    assert(!renderer.accepted && !renderer.prepared);
    assert(*renderer.prepare(key, capture, encode));
    assert(captured == 2 && encoded == 1);
    assert(!observer.cpuReleased());
    auto blocked = [](auto&) -> RenderResult<EFrameSubmit> { return EFrameSubmit::BACKPRESSURED; };
    assert(*renderer.submit(blocked) == lux::editor::views::EOverlaySubmit::BACKPRESSURED);
    assert(renderer.prepared == key && !renderer.accepted && !observer.cpuReleased());
    assert(*renderer.prepare(key, capture, encode));
    assert(captured == 2 && encoded == 1);
    TRenderProgram<> inflight;
    assert(*renderer.submit([&](auto& input) -> RenderResult<EFrameSubmit> {
        inflight = std::move(input);
        return EFrameSubmit::SUBMITTED;
    }) == lux::editor::views::EOverlaySubmit::ACCEPTED);
    assert(renderer.accepted == key && !renderer.prepared);
    auto gpu = observer.acquire();
    assert(gpu && gpu.record(9));
    gpu.submit(9);
    renderer.program.clear_keep_capacity();
    inflight.clear_keep_capacity();
    assert(!observer.complete());
    gpu.finish(9);
    gpu = {};
    assert(observer.complete());
    // Returning to the already displayed selection cancels a different backpressured candidate.
    key.selection = 20;
    assert(*renderer.prepare(key, capture, encode));
    assert(*renderer.submit(blocked) == lux::editor::views::EOverlaySubmit::BACKPRESSURED);
    auto abandoned = observer;
    key.selection = 12;
    const auto before_revert = captured;
    assert(!*renderer.prepare(key, capture, encode));
    assert(captured == before_revert && !renderer.prepared && abandoned.cpuReleased());
    key.selection = 13;
    assert(*renderer.prepare(key, capture, encode));
    auto replaced = observer;
    key.selection = 14;
    assert(*renderer.prepare(key, capture, encode));
    assert(replaced.cpuReleased());
    assert(!renderer.submit([](auto&) -> RenderResult<EFrameSubmit> {
        return lux::cxx::unexpected(RendererFailure{ERendererError::INVALID_ARGUMENT});
    }));
    assert(renderer.rejected == key && renderer.accepted->selection == 12 && !renderer.prepared);
    const auto count = captured;
    assert(!renderer.prepare(key, capture, encode));
    assert(captured == count);
    renderer.rejected.reset();
    assert(*renderer.prepare(key, capture, encode));
    renderer.program.clear_keep_capacity();
    assert(observer.cpuReleased());
    std::cout << "X07-01/02 actual preparation with controlled transport failure; real submission lifetime.\n";
}
