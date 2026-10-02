#include <lux/engine/editor/application/EditorApplicationImpl.hpp>
#include <lux/engine/editor/desktop/DesktopTestAccess.hpp>
#include <cassert>
#include <fstream>
#include <iostream>
#include <thread>

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
    const auto manifest = encodeProjectManifest({id, "Application", {}, {}, {}});
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
    assert(app->show(material_id, true)); // A second view is not a second working copy.
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
