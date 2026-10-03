#include "AssetSceneFixture.hpp"
#include <lux/engine/editor/scene/RunController.hpp>
#include <lux/engine/editor/scene/SceneView.hpp>
#include <lux/engine/editor/desktop/DesktopShell.hpp>
#include <lux/engine/ui/rendering/RenderFeature.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/process/asset_loading/AssetReadOverlay.hpp>

int main(int argc, char** argv)
{
    assert(argc == 3);
    Fixture f{argv[1], argv[2]};
    namespace e = lux::editor;
    namespace a = lux::editor::scene;
    auto renderer = take(render::RenderRuntime::create(render::RendererConfig{.validation = true}));
    const auto bounded = [&](auto condition, auto frame) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
        while (!condition()) {
            assert(std::chrono::steady_clock::now() < deadline);
            frame(); std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    };
    const auto render_frame = [&] {
        assert(renderer->collectCompletions(128));
        std::size_t controls = 128, programs = 32;
        assert(renderer->submitPending(controls, programs));
    };
    std::vector<render::RenderFeatureRegistration> features;
    for (const auto& value : render::builtinRenderFeatureRegistrations()) features.push_back(value);
    features.push_back(render::kUiRenderRenderFeatureRegistration);
    assert(renderer->beginFeatureRegistration(std::move(features)));
    bounded([&] { return renderer->featureRegistrationStatus().state != render::EFeatureRegistrationState::REGISTERING; }, render_frame);
    assert(renderer->commitFeatureRegistration());
    auto resources = take(lux::scene::RenderResources::create(*renderer, f.files, f.execution.cpu()));
    e::sessions::SessionStore authors{2};
    auto reservation = take(authors.reserve<a::SceneSession>({"lux.editor.scene"}, e::contracts::CodeLease::builtin()));
    auto model = take(a::SceneSession::create(reservation.id(), e::sessions::BoundSource{SceneId, "scene.lux"},
        take(a::SceneSource::create(f.package, f.schemas))));
    auto* author = model.get();
    assert(authors.prepare(reservation, model));
    const auto key = take(authors.key<a::SceneSession>(take(authors.publish(reservation))));
    const auto before = author->describe();
    const auto encoded = take(lux::scene::encodeScenePackage(take(a::buildSceneSnapshotPackage(take(author->capture()))), 1024*1024));
    a::RunStore runs{*f.runtime, f.execution, 2};
    a::RunController control{runs};
    a::ProjectionEnvironment environment{f.schemas, f.systems, {f.registrations.begin(), f.registrations.end()}};
    for (const auto& binding : lux::scene::builtinRenderFeatureSceneBindings()) environment.render_bindings.push_back(binding);
    environment.renderer = renderer.get(); environment.resources = resources.get();
    environment.assets = {{13,1}, 1, take(process::asset_loading::makeAssetReadOverlay({},{})), {}};
    a::RunEnvironment run_environment{environment.components, environment.simulation_systems,
        environment.scene_systems, environment.render_bindings, renderer.get(), resources.get(), environment.assets};
    run_environment.scripts = f.host;
    auto preparation = take(control.prepare(*author, std::move(run_environment), {std::chrono::milliseconds{1}, {3}}));
    const auto runtime_frame = [&] { render_frame(); f.frame(); assert(runs.update()); };
    bounded([&] { return preparation->ready(); }, runtime_frame);
    const auto run = take(control.adopt(*preparation)); preparation.reset();
    const auto instance = take(runs.info(run)).instance;
    assert(runs.pause(run));
    object::ObjectMessageQueue messages{take(object::ObjectMessageQueue::create(256))};
    a::ScenePresentationHub hub{*f.runtime, f.execution};
    auto desktop = take(e::desktop::DesktopShell::create(messages.dispatcherRef(), f.execution,
        *f.runtime, *renderer, *resources, nullptr, {.docking = false}));
    a::SceneViewServices services{authors.access<a::SceneSession>(), hub, *f.runtime, *resources, *renderer,
        environment, runs.inspect()};
    a::SceneInteractionGroup first{authors.access<a::SceneSession>(),key,{1},runs.inspect()};
    a::SceneInteractionGroup second{authors.access<a::SceneSession>(),key,{2},runs.inspect()};
    const auto create = [&](const char* name, a::SceneInteractionGroup& group) {
        a::SceneViewCreateInfo info{ui::PaneId{name}, name, a::RunningSceneBinding{run,&group}};
        info.render_system = {3}; info.state.camera.transform.translation = {0,3,8};
        return take(a::makeSceneView(messages.dispatcherRef(),services,std::move(info)));
    };
    auto left = create("script-left",first), right = create("script-right",second);
    auto* l = static_cast<a::SceneView*>(left.pane()); auto* r = static_cast<a::SceneView*>(right.pane());
    const auto lid = take(desktop->views().adopt(left,e::views::ViewRestoreKey{"script-left"})).id;
    const auto rid = take(desktop->views().adopt(right,e::views::ViewRestoreKey{"script-right"})).id;
    const auto frame = [&] {
        render_frame();
        assert(desktop->update(ui::FrameInfo{{1000,650},1.F/60.F}));
        f.frame(); assert(runs.update()); hub.collectReleased();
    };
    bounded([&] { return l->image().isValid() && r->image().isValid(); },frame);
    assert(l->presentedInstance() == instance && r->presentedInstance() == instance);
    assert(l->viewport() != r->viewport() && l->image() != r->image());
    assert(f.value(instance) == 7);
    assert(desktop->views().close(lid));
    bounded([&] { return !desktop->views().describe(lid); },frame);
    assert(r->presentedInstance() == instance && r->image().isValid());
    assert(take(runs.info(run)).state == a::ERunState::PAUSED);
    assert(runs.resume(run));
    bounded([&] { return f.value(instance) == 9; },frame);
    f.verifyFinished(instance);
    assert(f.backend.stats().prepared_ability_slots == 23);
    const auto after = author->describe();
    assert(after.current == before.current && after.observed == before.observed && after.dirty == before.dirty && after.binding == before.binding);
    assert(take(lux::scene::encodeScenePackage(take(a::buildSceneSnapshotPackage(take(author->capture()))),1024*1024)) == encoded);
    assert(runs.stop(run));
    bounded([&] { return take(runs.info(run)).state == a::ERunState::STOPPED; },frame);
    assert(runs.acknowledgeStop(run));
    assert(f.backend.stats().prepared_ability_slots == 0);
    assert(desktop->views().close(rid));
    bounded([&] { return !desktop->views().describe(rid); },frame);
    desktop.reset();
    bounded([&] { return resources->empty(); },runtime_frame);
    assert(f.files.join());
    assert(renderer->statistics().validation_errors == 0);
    assert(renderer->beginClose());
    bounded([&] { std::size_t a=128,b=128,c=32; return take(renderer->advanceClose(a,b,c)) == render::ERenderClose::COMPLETE; },runtime_frame);
    assert(renderer->joinStopped());
    std::puts("XEC2-32 installed: same packaged Lua + one Run + two real GPU SceneViews; close one, resume, stop, final code/GPU retirement; validation_errors=0 PASS");
}
