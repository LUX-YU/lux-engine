#include "AssetSceneFixture.hpp"
#include <lux/engine/editor/scene/RunController.hpp>
#include <lux/engine/editor/sessions/SessionStore.hpp>
int main(int argc, char** argv)
{
    assert(argc == 3);
    Fixture fixture{argv[1], argv[2]};
    using namespace lux::editor;
    namespace editing_scene = lux::editor::scene;
    sessions::SessionStore authors{2};
    auto reservation = take(authors.reserve<editing_scene::SceneSession>({"lux.editor.scene"}, lux::object::CodeLease::builtin()));
    auto author = take(editing_scene::SceneSession::create(reservation.id(),
        sessions::BoundSource{SceneId, "scene.lux"}, take(editing_scene::SceneSource::create(fixture.package, fixture.schemas))));
    auto* model = author.get();
    assert(authors.prepare(reservation, author) && authors.publish(reservation));
    const auto before = model->describe();
    const auto frozen_before = take(lux::scene::encodeScenePackage(
        take(editing_scene::buildSceneSnapshotPackage(take(model->capture()))), 1024 * 1024
    ));
    editing_scene::RunStore runs{*fixture.runtime, fixture.execution, 2};
    editing_scene::RunController control{runs};
    editing_scene::RunEnvironment environment{fixture.schemas, fixture.systems,
        {fixture.registrations.begin(), fixture.registrations.end()}};
    environment.scripts = fixture.host;
    auto preparation = take(control.prepare(*model, std::move(environment), {std::chrono::milliseconds{1}, {}}));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!preparation->ready() && std::chrono::steady_clock::now() < deadline)
    {
        fixture.frame();
        std::this_thread::yield();
    }
    assert(preparation->ready());
    const auto run = take(control.adopt(*preparation));
    const auto id = take(runs.info(run)).instance;
    fixture.frame(); // Starts the original Hook coroutine and its first real asset read.
    assert(fixture.value(id) == 7);
    assert(runs.pause(run));
    for (int frame{}; frame != 6; ++frame)
    {
        fixture.frame(); // Completed native work can settle while script evolution is paused.
        assert(runs.update());
        assert(fixture.value(id) == 7);
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    assert(runs.resume(run));
    for (int frame{}; frame != 30; ++frame)
    {
        fixture.frame();
        assert(runs.update());
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    assert(fixture.value(id) == 9);
    fixture.verifyFinished(id);
    const auto after = model->describe();
    assert(after.current == before.current && after.dirty == before.dirty);
    assert(after.observed == before.observed && after.binding == before.binding);
    assert(take(lux::scene::encodeScenePackage(take(editing_scene::buildSceneSnapshotPackage(take(model->capture()))),
        1024 * 1024)) == frozen_before);
    const auto stop = take(runs.stop(run));
    for (int frame{}; frame != 4; ++frame) { fixture.frame(); assert(runs.update()); }
    assert(take(runs.info(run)).state == editing_scene::ERunState::STOPPED);
    assert(runs.acknowledgeStop(run));
    assert(fixture.backend.stats().prepared_ability_slots == 0);
    assert(fixture.files.join());
    std::puts("EC2 actual SceneRuntime: packaged Lua/VFS, native assets, pause, command barrier, retirement PASS");
}
