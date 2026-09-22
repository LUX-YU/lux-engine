#include <lux/engine/render/RenderRuntime.hpp>

#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

int main()
{
    using namespace lux::render;
    using namespace std::chrono_literals;
    auto made = RenderRuntime::create({.validation = true});
    assert(made);
    auto runtime = std::move(*made);
    const auto pump = [&] {
        std::size_t controls = 4, programs = 1;
        assert(runtime->poll(16, controls, programs));
    };
    const auto until = [&](auto ready) {
        const auto deadline = std::chrono::steady_clock::now() + 15s;
        while (!ready())
        {
            assert(std::chrono::steady_clock::now() < deadline);
            pump();
            std::this_thread::sleep_for(1ms);
        }
    };
    auto scene = runtime->createScene({.name = "View failure owner"}, {});
    assert(scene);
    const auto receipt = scene->receipt();
    until([&] { return scene->status().state == ESceneResourceState::READY; });
    const auto scene_id = scene->id();
    auto opened = runtime->openView(*scene, {.extent = {64, 48}});
    assert(opened);
    auto view = std::move(*opened);
    until([&] { return view->status().state == EViewState::READY; });
    const auto old_id = view->id();
    const auto old_handle = view->handle();
    assert(view->requestExtent({96, 72}));
    until([&] { return view->status().state == EViewState::FAILED; });
    const auto original = view->status();
    assert(original.failure && original.failure->view == old_id && original.failure->request != 0);
    assert(original.failure->backend_status == 3 && original.ready_extent == (PixelExtent{64, 48}));
    const auto retry = view->requestExtent({128, 96});
    const auto hidden = view->requestExtent({0, 0});
    assert(!retry && !hidden && retry.error().request == original.failure->request &&
           hidden.error().backend_status == original.failure->backend_status);
    assert(view->status().state == EViewState::FAILED && !view->acquireImage());
    assert(view->beginClose());
    until([&] { return view->status().state == EViewState::CLOSED; });
    assert(view->status().failure->request == original.failure->request);
    view.reset();
    assert(scene->id() == scene_id && scene->status().state == ESceneResourceState::READY);
    opened = runtime->openView(*scene, {.extent = {96, 72}});
    assert(opened);
    view = std::move(*opened);
    until([&] { return view->status().state == EViewState::READY; });
    assert(view->id() != old_id && view->handle() != old_handle);
    assert(view->status().ready_extent == (PixelExtent{96, 72}));
    assert(view->acquireImage());
    view.reset();
    *scene = {};
    until([&] { return receipt.status().state == ESceneResourceState::RETIRED; });
    assert(runtime->statistics().views == 0 && runtime->statistics().runtime_leases == 0);
    assert(runtime->statistics().validation_errors == 0);
    assert(runtime->beginClose());
    until([&] {
        std::size_t replies = 16, controls = 4, programs = 1;
        const auto closed = runtime->advanceClose(replies, controls, programs);
        assert(closed);
        return *closed == ERenderClose::COMPLETE;
    });
    assert(runtime->joinStopped());
    std::cout << "PASS diagnostic backend resize rejection: original status=3/request preserved, "
                 "FAILED unaffected by resize/hide, old View CLOSED before new identity, same Scene, "
                 "new output READY, all owners retired; not physical device loss or desktop input\n";
}
