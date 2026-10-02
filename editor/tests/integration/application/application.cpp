#include <lux/engine/editor/application/EditorApplicationImpl.hpp>
#include <lux/engine/editor/desktop/DesktopTestAccess.hpp>
#include <lux/engine/editor/scene/OutlinerView.hpp>
#include <cassert>
#include <fstream>
#include <iostream>
#include <thread>
#include <algorithm>

namespace lux::editor::application
{
    struct ApplicationTestAccess final
    {
        static auto& implementation(EditorApplication& app) noexcept
        {
            return *app.impl_;
        }
    };
}
int main(int argc, char** argv)
{
    using namespace lux;
    using namespace lux::editor;
    using namespace lux::editor::application;
    assert(argc == 3);
    const auto root =
        std::filesystem::path{argv[2]} / std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(root);
    const auto id = asset::AssetId{*uuids::uuid::from_string("57279371-1b9c-40d6-b55d-a1a12065d932")};
    const auto manifest = encodeProjectManifest({id, "Application", {}, {}, {{"lux.builtin.scene_render", 1, {}}}});
    assert(manifest);
    const auto file = root / "Project.luxproject";
    {
        std::ofstream output(file, std::ios::binary);
        output << *manifest;
        assert(output);
    }
    EditorApplicationConfig config{file, argv[1], "Application qualification", 640, 480, true};
    desktop::testing::rejectNextMenuConnection();
    auto rejected = EditorApplication::create(config);
    if (!rejected)
        std::cerr << "Application construction: " << rejected.error().domain << ": " << rejected.error().message
                  << '\n';
    assert(!rejected && rejected.error().domain == "object.connect");
    const auto* cause = std::any_cast<commands::CommandFailure>(&rejected.error().cause);
    assert(
        cause && cause->domain == "object.connect" &&
        cause->domain_code == static_cast<std::uint64_t>(object::EConnectError::CAPACITY_EXHAUSTED)
    );
    std::cout << "C04: actual EditorApplication::create rejects required menu connection failure\n";
    auto created = EditorApplication::create(config);
    if (!created)
    {
        std::cerr << created.error().domain << '\n';
        return 1;
    }
    auto app = std::move(*created);
    auto& impl = ApplicationTestAccess::implementation(*app);
    assert(impl.desktop_ && impl.desktop_->commands());
    for (int frame = 0; frame < 8; ++frame)
        assert(app->update());
    assert(impl.sessions_.size() == 0);
    auto views = impl.desktop_->views().describeAll();
    assert(views && views->size() == 2);
    const auto hidden = views->front().id;
    assert(impl.desktop_->views().hide(hidden));
    assert(impl.desktop_->views().drain());
    const auto layout_id = workspace::LayoutId{"572793711b9c40d6b55da1a12065d932"};
    auto before_layout = impl.desktop_->views().captureLayout(layout_id, "Original");
    assert(before_layout);
    const auto before_bytes = workspace::encodeLayout(*before_layout);
    assert(before_bytes);
    auto invalid_layout = *before_layout;
    invalid_layout.slots.front().visible = true;
    invalid_layout.dock.roots.push_back({999999});
    auto invalid = app->applyLayout(std::move(invalid_layout));
    assert(!invalid && !impl.desktop_->views().describe(hidden)->visible);
    auto after_layout = impl.desktop_->views().captureLayout(layout_id, "Original");
    assert(after_layout);
    auto after_bytes = workspace::encodeLayout(*after_layout);
    assert(after_bytes && *after_bytes == *before_bytes);
    assert(impl.desktop_->views().describeAll()->size() == views->size());
    assert(app->applyLayout(*before_layout));
    std::cout << "C01: malformed product layout preserves hidden state, count and exact dock encoding\n";
    const auto create_content = [&](const char* command) {
        auto accepted = app->execute(commands::CommandId{command});
        assert(accepted);
        auto operation = std::get<commands::AcceptedOperation>(*accepted);
        assert(operation.kind == "open");
        const sessions::OpenAssetId open{operation.value};
        const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (true)
        {
            assert(std::chrono::steady_clock::now() < limit);
            assert(app->update());
            const auto status = app->openStatus(open);
            assert(status && !status->presentation_failure);
            if (status->view)
            {
                const auto id = status->content.session;
                assert(impl.sessions_.describe(id)->dirty);
                assert(app->acknowledgeOpen(open));
                return id;
            }
        }
    };
    const auto material_id = create_content("lux.editor.new.material");
    const auto flow_id = create_content("lux.editor.new.flow");
    assert(impl.sessions_.size() == 2);
    assert(app->execute(
        commands::CommandId{"lux.editor.another-view"},
        commands::CommandInvocation{commands::SessionTarget{material_id, impl.sessions_.describe(material_id)->current}}
    ));
    // The real command creates a second view under its existing dispatch protection, not a working copy.
    assert(impl.sessions_.size() == 2);
    const auto material_before = impl.sessions_.describe(material_id)->current;
    const auto flow_before = impl.sessions_.describe(flow_id)->current;
    const auto answer = [&](desktop::EReviewChoice choice) {
        assert(impl.review_);
        auto choose = [&](ui::Pane& pane) {
            assert(pane.type() == ui::PaneTypeId{"lux.editor.review"});
            assert(static_cast<desktop::ReviewView&>(pane).answer(choice));
        };
        assert(impl.desktop_->views().withView(*impl.review_, choose));
    };
    assert(app->requestExit());
    assert(app->update() && impl.review_);
    answer(desktop::EReviewChoice::CANCEL);
    assert(app->update() && app->phase() == EApplicationPhase::RUNNING);
    assert(impl.sessions_.describe(material_id)->current == material_before);
    assert(impl.sessions_.describe(flow_id)->current == flow_before);
    assert(impl.sessions_.size() == 2);
    std::cout << "Actual new commands, shared content views and cancel exit preserve all author state\n";
    const auto choose_last = [&](desktop::EReviewChoice choice) {
        assert(impl.last_view_);
        auto choose = [&](ui::Pane& pane) { assert(static_cast<desktop::ReviewView&>(pane).answer(choice)); };
        assert(impl.desktop_->views().withView(impl.last_view_->question, choose));
        assert(app->update());
    };
    std::vector<views::ViewId> material_views;
    for (const auto& view : impl.content_views_)
        if (view.session == material_id)
            material_views.push_back(view.view);
    assert(material_views.size() == 2);
    assert(app->closeView(material_views.back()));
    assert(!impl.last_view_ && impl.sessions_.describe(material_id)->current == material_before);
    assert(app->closeView(material_views.front()) && impl.last_view_);
    choose_last(desktop::EReviewChoice::CANCEL);
    assert(impl.desktop_->views().describe(material_views.front()));
    assert(app->closeView(material_views.front()));
    choose_last(desktop::EReviewChoice::KEEP_CONTENT);
    assert(!impl.desktop_->views().describe(material_views.front()));
    assert(impl.sessions_.describe(material_id)->current == material_before);
    const auto shown_again = app->show(material_id);
    assert(shown_again && *shown_again != material_views.front());
    auto flow_view =
        std::ranges::find_if(impl.content_views_, [&](const auto& record) { return record.session == flow_id; });
    assert(flow_view != impl.content_views_.end());
    const auto flow_view_id = flow_view->view;
    assert(app->closeView(flow_view_id));
    choose_last(desktop::EReviewChoice::CLOSE_CONTENT);
    assert(impl.review_ && impl.sessions_.size() == 2);
    answer(desktop::EReviewChoice::DISCARD);
    assert(app->update() && app->phase() == EApplicationPhase::RUNNING);
    assert(!impl.sessions_.describe(flow_id) && !impl.desktop_->views().describe(flow_view_id));
    assert(impl.sessions_.describe(material_id)->current == material_before);
    std::cout << "Last-view Cancel/Keep/Close use real modals; closing one content preserves another\n";

    auto new_scene = app->execute(commands::CommandId{"lux.editor.new.scene"});
    if (!new_scene)
        std::cerr << "New Scene: " << new_scene.error().domain << ": " << new_scene.error().detail << '\n';
    assert(new_scene);
    auto windows = impl.desktop_->views().describeAll();
    assert(windows);
    auto creation = std::ranges::find_if(*windows, [](const auto& view) {
        return view.type == views::ViewTypeId{"lux.editor.scene.creation"};
    });
    assert(creation != windows->end());
    auto configure_scene = [&](ui::Pane& pane) {
        auto& form = static_cast<lux::editor::scene::SceneCreationView&>(pane);
        const auto preset =
            form.configuration().applyPreset(lux::editor::scene::ESceneContentPreset::THREE_DIMENSIONAL);
        if (!preset)
            std::cerr << preset.error().domain << ": " << preset.error().message << '\n';
        assert(preset);
        form.requestCreate();
    };
    assert(impl.desktop_->views().withView(creation->id, configure_scene));
    sessions::SessionId scene_id;
    const auto scene_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!scene_id.valid())
    {
        assert(std::chrono::steady_clock::now() < scene_deadline);
        auto updated = app->update();
        if (!updated)
            std::cerr << updated.error().domain << '\n';
        assert(updated);
        for (const auto& record : impl.content_views_)
            if (record.scene && !record.run)
                scene_id = record.session;
    }
    auto scene_record = std::ranges::find(
        impl.content_views_,
        scene_id,
        &std::remove_reference_t<decltype(impl)>::ContentView::session
    );
    assert(scene_record != impl.content_views_.end());
    const auto scene_view = scene_record->view;
    assert(app->execute(commands::CommandId{"lux.editor.scene.outliner"}, commands::CommandInvocation{scene_view}));
    auto all_views = impl.desktop_->views().describeAll();
    assert(all_views);
    auto outliner = std::ranges::find_if(*all_views, [](const auto& view) {
        return view.type == views::ViewTypeId{"lux.editor.outliner"};
    });
    assert(outliner != all_views->end());
    const world::WorldObjectId object{*uuids::uuid::from_string("49606a4d-f15c-4e2f-a6e7-a25efb3f6a10")};
    auto create_object = [&](ui::Pane& pane) {
        assert(static_cast<lux::editor::scene::OutlinerView&>(pane)
                   .createObject(object, {0}, lux::editor::scene::EObjectSpace::SPACE_3D));
    };
    assert(impl.desktop_->views().withView(outliner->id, create_object));
    const auto run_source = impl.sessions_.describe(scene_id)->current;
    assert(app->execute(
        commands::CommandId{"lux.editor.play"},
        commands::CommandInvocation{commands::SessionTarget{scene_id, run_source}}
    ));
    while (impl.run_presentations_.empty() || !impl.run_presentations_.front().run)
    {
        assert(std::chrono::steady_clock::now() < scene_deadline);
        auto updated = app->update();
        if (!updated)
            std::cerr << updated.error().domain << '\n';
        assert(updated);
    }
    const auto run = *impl.run_presentations_.front().run;
    assert(!impl.run_presentations_.front().failure);
    auto run_record = std::ranges::find_if(impl.content_views_, [&](const auto& view) { return view.run == run; });
    assert(run_record != impl.content_views_.end());
    const auto run_view = run_record->view;
    assert(app->execute(commands::CommandId{"lux.editor.scene.pause"}, commands::CommandInvocation{run_view}));
    while (impl.runs_.info(run)->pause_pending)
    {
        assert(app->update());
    }
    assert(app->execute(commands::CommandId{"lux.editor.scene.step"}, commands::CommandInvocation{run_view}));
    const auto step = impl.run_presentations_.front().steps.front();
    for (int frame = 0; frame < 16; ++frame)
        assert(app->update());
    assert(impl.runs_.stepStatus(step)); // Completion has not been silently acknowledged by an update.
    assert(impl.requestClose(impl.sessions_.describe(scene_id)->current));
    assert(app->update() && impl.review_);
    answer(desktop::EReviewChoice::DISCARD);
    assert(app->update() && impl.review_ && impl.review_run_ == run);
    answer(desktop::EReviewChoice::KEEP_RUN);
    assert(app->update() && app->phase() == EApplicationPhase::RUNNING);
    assert(!impl.sessions_.describe(scene_id) && impl.runs_.info(run));
    assert(impl.desktop_->views().describe(run_view));
    assert(impl.run_presentations_.front().interaction->synchronize());
    assert(app->execute(commands::CommandId{"lux.editor.scene.outliner"}, commands::CommandInvocation{run_view}));
    assert(app->execute(commands::CommandId{"lux.editor.scene.resume"}, commands::CommandInvocation{run_view}));
    for (int frame = 0; frame < 4; ++frame)
        assert(app->update());
    assert(impl.runs_.info(run)->provenance.content == run_source);
    std::cout << "Formal scene form, Outliner, frozen Play/Pause/Step/Resume and Keep Run after author close\n";

    assert(app->requestExit());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (app->phase() != EApplicationPhase::RELEASED)
    {
        assert(std::chrono::steady_clock::now() < deadline);
        if (impl.review_)
            answer(desktop::EReviewChoice::DISCARD);
        auto updated = app->update();
        if (!updated)
        {
            std::cerr << updated.error().domain << '\n';
            return 1;
        }
        std::this_thread::yield();
    }
    assert(!impl.desktop_ && impl.engine_->renderContext()->resources().empty());
    std::cout << "Application: formal service assembly, frames, and asynchronous exit drain complete\n";
}
