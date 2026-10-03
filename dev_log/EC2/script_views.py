"""Installed-only joint XEC2-32: the packaged Lua Run and two actual GPU SceneViews."""
from pathlib import Path
import json, subprocess, sys
w=Path(__file__).resolve().parent;c=json.loads((w/'final-config.json').read_text())
s=Path(c['source']);prefix=Path(c['prefix']);root=w/'script-views';root.mkdir(exist_ok=True)
build=Path(c['build']).with_name('ec2-final-script-views')
label=sys.argv[1] if len(sys.argv)>1 else 'final-script-views'
# Reuse the installed-consumer generator/package recipe, not generated build-tree output.
cmake=(s/'cmake/installed-consumers/editor-ec2/CMakeLists.txt').read_text()
cmake=cmake.replace('get_filename_component(engine_source "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)',
    'set(engine_source "'+s.as_posix()+'")\nset(EC2_MODE SCENE)')
cmake=cmake.replace('fixture(engine/scene/integration/script/test/AssetSceneFixture.hpp AssetSceneFixture.hpp)',
    'configure_file("${CMAKE_CURRENT_SOURCE_DIR}/AssetSceneFixture.hpp" "${CMAKE_CURRENT_BINARY_DIR}/AssetSceneFixture.hpp" COPYONLY)')
cmake=cmake.replace('fixture(engine/scene/integration/script/test/asset_scene.cpp asset_scene.cpp)',
    'configure_file("${CMAKE_CURRENT_SOURCE_DIR}/main.cpp" "${CMAKE_CURRENT_BINARY_DIR}/asset_scene.cpp" COPYONLY)')
cmake=cmake[:cmake.index('add_library(ec2_constraints')]
cmake+='''
find_package(lux-engine-editor-desktop REQUIRED COMPONENTS desktop_shell)
find_package(lux-engine-editor-scene-ui REQUIRED COMPONENTS scene_ui)
find_package(lux-engine-editor-scene-execution REQUIRED COMPONENTS scene_execution)
find_package(lux-engine-function REQUIRED COMPONENTS ui_rendering render_features)
target_link_libraries(ec2_consumer PRIVATE lux::engine::editor::scene_ui lux::engine::editor::scene_execution
    lux::engine::editor::desktop_shell lux::engine::function::ui_rendering lux::engine::function::render_features)
'''
(root/'CMakeLists.txt').write_text(cmake)
fixture=(s/'engine/scene/integration/script/test/AssetSceneFixture.hpp').read_text()
fixture=fixture.replace('#pragma once', '''#pragma once
#include <lux/engine/scene/TransformSystem.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/RenderSystemConfiguration.hpp>
#include <lux/engine/scene/Builtin3DRenderIntegration.hpp>
#include <lux/engine/function/render/features/BuiltinFeatures.hpp>''')
fixture=fixture.replace('std::array<lux::scene::SceneSystemRegistration, 1> registrations{\n            observedScriptRegistration()',
    'std::array<lux::scene::SceneSystemRegistration, 3> registrations{\n            observedScriptRegistration(), lux::scene::transformSystemRegistration(), lux::scene::builtinRenderSystemRegistration()')
anchor='            package = take(lux::scene::createScenePackage'
extra='''            const auto add = [&](std::size_t index, std::span<const std::byte> bytes) {
                const auto& r = registrations[index];
                assert(scene.addSystem({index + 1}, std::to_string(index), r.type, r.description->version,
                    r.description->configuration_schema_name, r.description->configuration_schema_version, bytes));
            };
            add(1, take(lux::scene::makeTransformSystemConfiguration(64, {1024, 65536})));
            lux::scene::RenderSystemConfiguration render_configuration;
            for (const auto& feature : render::builtinRenderFeatureRegistrations())
            {
                const auto name = feature.factory.descriptor.canonical_name;
                if (name.find("camera") == std::string_view::npos && name.find("grid") == std::string_view::npos)
                    continue;
                std::vector<std::byte> defaults;
                assert(feature.configuration.portable.encode_default(defaults));
                render_configuration.features.push_back({feature.factory.descriptor.type, std::move(defaults),
                    std::string(feature.configuration.schema), feature.configuration.schema_version});
            }
            std::vector<std::byte> render_bytes;
            assert(registrations[2].configuration.encode(&render_configuration, render_bytes));
            add(2, render_bytes);
            assert(scene.bindRequirement({3}, "render_runtime", "main-window"));
            assert(scene.bindRequirement({3}, "render_bindings", "render-bindings"));
            assert(scene.bindRequirement({3}, "render_resources", "resources"));
'''
assert anchor in fixture and 'Registration, 3>' in fixture
fixture=fixture.replace('runtime->borrowInstance(id)', 'std::as_const(*runtime).borrowInstance(id)')
(root/'AssetSceneFixture.hpp').write_text(fixture.replace(anchor,extra+anchor))
(root/'main.cpp').write_text(r'''#include "AssetSceneFixture.hpp"
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
    bounded([&] {
        const auto& registry = take(std::as_const(*f.runtime).borrowInstance(instance)).get();
        const auto& scripts = registry.ctx().get<ScriptObservation>().runtime->scriptSystem();
        return scripts.activeContinuationCount() == 0 && scripts.activeAwaitableCount() == 0;
    },frame);
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
''')
for name,args in [('configure',['cmake','-S',root,'-B',build,'-G','Ninja','-DCMAKE_BUILD_TYPE=RelWithDebInfo',
    '-DCMAKE_PREFIX_PATH='+prefix.as_posix(),'-DSDK_PREFIX='+prefix.as_posix(),
    '-DLuxLua55_DIR=E:/SyncForder/CodeRepos/install/o/v4/lua55/lib/cmake/LuxLua55',
    '-DCMAKE_TOOLCHAIN_FILE=D:/Development/vcpkg/scripts/buildsystems/vcpkg.cmake']),
    ('build',['cmake','--build',build,'--target','all','-j','4','--','-k','0']),
    ('no-work',['cmake','--build',build,'--target','all','-j','4','--','-k','0']),
    ('test',['ctest','--test-dir',build,'--output-on-failure','-j','1'])]:
    subprocess.run([sys.executable,str(w/'run.py'),'--runtime',str(prefix/'bin'),'--cwd',str(s),
        label+'-'+name,*map(str,args)],check=True)
