#include <array>
#include <lux/engine/material/graph/Nodes.hpp>
#include <lux/engine/editor/application/EditorApplicationImpl.hpp>
#include <lux/engine/editor/desktop/DesktopTestAccess.hpp>
#include <lux/engine/editor/scene/OutlinerView.hpp>
#include <lux/engine/editor/scene/SceneTools.hpp>
#include <lux/engine/editor/project/ImportView.hpp>
#include <lux/engine/editor/project/SettingsView.hpp>
#include <lux/engine/editor/scene/InspectorView.hpp>
#include <lux/engine/editor/storage/FilePublication.hpp>
#include <lux/engine/editor/scene/ResourceView.hpp>
#include <lux/engine/scene/SceneSystemInstaller.hpp>
#include <lux/engine/resource/asset/model/ModelAsset.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>
#include <lux/engine/material/Cooker.hpp>
#include <lux/engine/resource/asset/AssetSerDeser.hpp>
#include <lux/engine/resource/asset/storage/pak/PakArchive.hpp>
#include <lux/engine/ui/Layout.hpp>
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
namespace
{
    std::vector<lux::editor::views::ViewInfo> contentViews(lux::editor::desktop::ViewHost& host,
                                                        lux::editor::sessions::SessionId session)
    {
        auto all = host.describeAll();
        assert(all);
        std::erase_if(*all, [&](const auto& view) {
            return std::ranges::find(view.content.sessions, session) == view.content.sessions.end();
        });
        return std::move(*all);
    }
    class ComparisonPane final : public lux::ui::Pane
    {
    public:
        ComparisonPane(lux::object::ObjectDispatcherRef dispatcher, lux::ui::PaneId id,
                       lux::editor::views::ViewContent content)
            : Pane(dispatcher, std::move(id), lux::ui::PaneTypeId{"test.comparison"}, "Comparison"),
              content_(std::move(content))
        {}
        [[nodiscard]] const lux::editor::views::ViewContent& content() const noexcept { return content_; }
    private:
        lux::editor::views::ViewContent content_;
    };
    struct FailureSystem final
    {
        inline static constexpr std::string_view worlds[]{"*"};
        inline static constexpr lux::system::SystemTypeDescription Description{
            .canonical_name = "test.application.failure",
            .version = 1,
            .supported_world_types = worlds
        };
    };
    lux::scene::SceneSystemRegistration failureRegistration()
    {
        using namespace lux;
        return {
            .type = system::systemTypeId(FailureSystem::Description.canonical_name),
            .cpp_type = cxx::typeToken<FailureSystem>(),
            .description = &FailureSystem::Description,
            .install = +[](scene::SceneSystemInstaller& installer, scene::SceneSystemDescription description
                        ) noexcept -> cxx::expected<void, scene::SceneSystemBuildFailure> {
                auto installed = installer.emplaceSystem<FailureSystem>(description.instanceId());
                if (!installed)
                    return cxx::unexpected(installed.error());
                return installer.addPublicationTask<FailureSystem>(
                    description.instanceId(),
                    [](FailureSystem&) noexcept -> scene::SceneStageResult {
                        return cxx::unexpected(
                            scene::SceneExecutionFailure{scene::ESceneExecutionError::SYSTEM_FAILURE, {}, 731}
                        );
                    }
                );
            }
        };
    }
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
    const auto model_id = asset::AssetId{*uuids::uuid::from_string("4789f937-0886-4aab-91d9-d45b4a9cffb4")};
    const auto mesh_id = asset::AssetId{*uuids::uuid::from_string("5889f937-0886-4aab-91d9-d45b4a9cffb4")};
    const auto material_asset_id = asset::AssetId{*uuids::uuid::from_string("6889f937-0886-4aab-91d9-d45b4a9cffb4")};
    auto mesh_data = std::make_shared<rdesc::Mesh>();
    mesh_data->vertices.resize(3);
    mesh_data->vertices[0].position = {-1, 0, 0};
    mesh_data->vertices[1].position = {1, 0, 0};
    mesh_data->vertices[2].position = {0, 1, 0};
    for (auto& vertex : mesh_data->vertices)
    {
        vertex.normal = {0, 0, 1};
        vertex.tangent = {1, 0, 0};
        vertex.bitangent = {0, 1, 0};
        vertex.uv = {0, 0};
        for (auto& bone : vertex.bone.bone_ids)
            bone = -1;
    }
    mesh_data->indices = {0, 1, 2};
    mesh_data->bounds = math::AABB{{-1, 0, 0}, {1, 1, 0}};
    auto mesh_asset = asset::MeshAsset::create({mesh_id, asset::MeshAsset::asset_type}, mesh_data);
    auto material_asset = lux::material::cookImportedMaterial(
        {material_asset_id, asset::MaterialAsset::asset_type},
        lux::material::ImportedMaterialDescription{}
    );
    assert(mesh_asset && material_asset);
    auto model_description = std::make_shared<rdesc::ModelDescription>();
    model_description->primitives.push_back({mesh_id, material_asset_id});
    model_description->nodes.resize(2);
    model_description->nodes[1].primitives.push_back(0);
    model_description->nodes[0].children.push_back(1);
    auto model_asset = asset::ModelAsset::create({model_id, asset::ModelAsset::asset_type}, model_description);
    assert(model_asset);
    std::vector<asset::PakWriteEntry> entries;
    const auto append = [&]<class Asset>(const Asset& value, std::string path) {
        auto encoded = asset::TAssetSerDeser<Asset>::encode(value, asset::AssetEncodeLimits{16 * 1024 * 1024});
        assert(encoded);
        auto bytes = std::make_shared<const std::vector<std::byte>>(std::move(*encoded));
        entries.push_back(
            {value.id(), Asset::primary_magic, std::move(path), {}, cxx::SharedBytes<>::fromOwner(bytes, *bytes)}
        );
    };
    append(**model_asset, "Content/Model");
    append(**mesh_asset, "Content/Model/Mesh");
    append(**material_asset, "Content/Model/Material");
    std::filesystem::create_directories(root / "Content");
    assert(asset::writePakFile(root / "Content/Model.pak", std::move(entries), "/Project"));
    {
        std::ofstream source(root / "Content/Model.recipe");
        source << "model fixture";
    }
    const auto manifest = encodeProjectManifest(
        {id,
         "Application",
         {},
         {{model_id, "lux.model.source", "Content/Model.recipe", "Content/Model.pak", {}, {}, "Content/Model"}},
         {{"lux.builtin.scene_render", 1, {}}}}
    );
    assert(manifest);
    const auto file = root / "Project.luxproject";
    {
        std::ofstream output(file, std::ios::binary);
        output << *manifest;
        assert(output);
    }
    EditorApplicationConfig config{file, argv[1], "Application qualification", 640, 480, true};
    config.user_directory = root.parent_path() / (root.filename().string() + "-user");
    std::filesystem::create_directories(*config.user_directory);
    config.font = root / "missing-font.ttf";
    auto missing_font = EditorApplication::create(config);
    if (!missing_font)
    {
        std::cerr << "Missing-font construction: " << missing_font.error().domain << '\n';
        if (const auto* detail = std::any_cast<workspace::WorkspaceFailure>(&missing_font.error().cause))
            std::cerr << "workspace: " << int(detail->code) << ' ' << detail->detail << '\n';
        if (const auto* detail = std::any_cast<settings::SettingsFailure>(&missing_font.error().cause))
            std::cerr << "settings: " << int(detail->code) << ' ' << detail->detail << '\n';
    }
    assert(!missing_font && missing_font.error().domain == "editor.font.read");
    config.font.reset();
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
    {
        auto direct = EditorApplication::create(config);
        assert(direct);
        for (const auto command : {"lux.editor.new.material", "lux.editor.new.flow", "lux.editor.settings"})
            assert((*direct)->execute(commands::CommandId{command}));
        for (int frame{}; frame < 8; ++frame)
            assert((*direct)->update());
        auto& owned = ApplicationTestAccess::implementation(**direct);
        assert(owned.sessions_.size() == 2 && owned.desktop_->views().describeAll()->size() >= 5);
        const auto session_ids = *owned.sessions_.snapshotIds();
        std::vector<sessions::SessionInfo> content_before;
        for (const auto session : session_ids)
            content_before.push_back(*owned.sessions_.describe(session));
        const auto views_before = *owned.desktop_->views().describeAll();
        const auto revision_before = owned.commands_.revision();
        const auto check_foreign_commands = [&] {
            // The owner is waiting; no UI, Store or command mutation runs concurrently.
            std::thread foreign([&] {
                for (const auto command : {"lux.editor.new.material", "lux.editor.exit", "test.missing"})
                {
                    const auto result = (*direct)->execute(commands::CommandId{command});
                    assert(!result);
                    std::cout << "P13 foreign command " << command << " code=" << int(result.error().code)
                              << " domain=" << result.error().domain << std::endl;
                    assert(result.error().code == commands::ECommandError::WRONG_THREAD);
                    assert(result.error().domain == "application.thread");
                }
            });
            foreign.join();
        };
        check_foreign_commands();
        {
            // Reentry is BUSY on the owner, but wrong-thread admission still takes precedence.
            std::remove_reference_t<decltype(owned)>::Dispatch scope{owned.dispatching_};
            const auto nested = (*direct)->execute(commands::CommandId{"lux.editor.new.material"});
            assert(!nested && nested.error().code == commands::ECommandError::BUSY);
            assert(nested.error().domain == "application.dispatch");
            check_foreign_commands();
            assert(owned.dispatching_);
        }
        assert(!owned.dispatching_ && (*direct)->phase() == EApplicationPhase::RUNNING);
        assert(owned.commands_.revision() == revision_before);
        assert(*owned.sessions_.snapshotIds() == session_ids);
        const auto views_after = *owned.desktop_->views().describeAll();
        assert(views_after.size() == views_before.size());
        for (std::size_t i{}; i < views_before.size(); ++i)
            assert(views_after[i].id == views_before[i].id);
        for (const auto& before : content_before)
        {
            const auto after = owned.sessions_.describe(before.id);
            assert(after && after->current == before.current && after->observed == before.observed);
            assert(after->binding == before.binding && after->dirty == before.dirty);
            assert(after->admission == before.admission);
        }
        const auto missing = (*direct)->execute(commands::CommandId{"test.missing"});
        assert(!missing && missing.error().code == commands::ECommandError::NOT_FOUND);
        assert((*direct)->execute(commands::CommandId{"lux.editor.assets"}));
        const auto about = (*direct)->execute(commands::CommandId{"lux.editor.about"});
        assert(!about && about.error().code == commands::ECommandError::DISABLED);
        std::cout << "P13 command admission: owner recovers; sessions, content and views unchanged\n";
        // No exec/requestExit: destruction still releases mounted UI and accepted resource work.
    }
    std::cout << "Application direct destruction: formal content, tools and GPU presentation released\n";
    auto created = EditorApplication::create(config);
    if (!created)
    {
        std::cerr << created.error().domain << '\n';
        return 1;
    }
    auto app = std::move(*created);
    auto& impl = ApplicationTestAccess::implementation(*app);
    assert(impl.desktop_ && impl.desktop_->commands());
    assert(app->execute(commands::CommandId{"lux.editor.project.recent"}));
    const auto recent_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!impl.recent_projects_->publication())
    {
        assert(app->update());
        assert(std::chrono::steady_clock::now() < recent_deadline);
        std::this_thread::yield();
    }
    assert(std::holds_alternative<persistence::CommitReceipt>(*impl.recent_projects_->publication()));
    assert(impl.recent_projects_->entries().size() == 1 &&
        std::filesystem::equivalent(impl.recent_projects_->entries().front(), config.project_file));
    const auto recent_file = *config.user_directory / "lux/editor/recent-projects.toml";
    const auto recent_before = storage::readPublicationFile(recent_file, 64 * 1024);
    assert(recent_before && !recent_before->empty());
    const std::string bad_recent = "version = 9\nprojects = []\n";
    assert(storage::writePublicationFile(recent_file, std::as_bytes(std::span{bad_recent})));
    assert(impl.recent_projects_->refresh());
    while (!impl.recent_projects_->failure())
    {
        assert(app->update());
        assert(std::chrono::steady_clock::now() < recent_deadline);
    }
    assert(impl.recent_projects_->failure()->domain == "recent.format" && impl.recent_projects_->entries().size() == 1);
    const auto refused = storage::readPublicationFile(recent_file, 64 * 1024);
    assert(refused && std::string(reinterpret_cast<const char*>(refused->data()), refused->size()) == bad_recent);
    assert(storage::writePublicationFile(recent_file, *recent_before));
    assert(impl.recent_projects_->refresh());
    do
    {
        assert(app->update());
        assert(std::chrono::steady_clock::now() < recent_deadline);
    } while (impl.recent_projects_->failure() || !impl.recent_projects_->settled());
    assert(
        impl.recent_projects_->entries().size() == 1 &&
        !impl.files_.resolve((root.parent_path() / "outside-user-root.txt").generic_string())
    );
    std::cout << "Recent projects: real legacy-format read, one coordinator publication, malformed input preserves "
                 "file/list, explicit retry\n";

    auto tools = impl.desktop_->views().describeAll();
    assert(tools);
    auto recent_view =
        std::ranges::find(*tools, views::ViewTypeId{"lux.editor.recent-projects"}, &views::ViewInfo::type);
    assert(recent_view != tools->end());
    auto close_recent = impl.desktop_->views().prepareClose(std::span{&recent_view->id, 1});
    assert(close_recent && impl.desktop_->views().commit(*close_recent));
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
    using ApplicationImpl = std::remove_reference_t<decltype(impl)>;
    const auto settle_workspace = [&] {
        const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::ranges::any_of(impl.workspace_changes_.publications(), [](const auto& item) { return !item.result; }))
        {
            assert(std::chrono::steady_clock::now() < limit);
            assert(app->update());
            std::this_thread::yield();
        }
        for (const auto& report : impl.workspace_changes_.publications())
            assert(report.result && std::holds_alternative<persistence::CommitReceipt>(*report.result));
    };
    assert(app->execute(commands::CommandId{"lux.editor.workspace"}));
    assert(app->update());
    assert(impl.executeWorkspaceIntent(lux::editor::project::SaveLayout{"Quality workspace"}));
    settle_workspace();
    assert(impl.workspace_changes_.catalog().layouts.size() == 1);
    const auto stored_id = impl.workspace_changes_.catalog().layouts.front().id;
    auto stored = impl.workspace_.readLayout(stored_id);
    assert(stored && stored->value.label == "Quality workspace");
    assert(impl.executeWorkspaceIntent(lux::editor::project::RenameLayout{stored_id, "Renamed"}));
    settle_workspace();
    auto renamed = impl.workspace_.readLayout(stored_id);
    assert(renamed && renamed->value.id == stored_id && renamed->value.label == "Renamed");
    assert(renamed->target.key == stored->target.key);
    assert(impl.desktop_->views().show(views->front().id));
    assert(impl.executeWorkspaceIntent(lux::editor::project::ApplyLayout{stored_id}));
    assert(!impl.desktop_->views().describe(views->front().id)->visible);
    settle_workspace();
    assert(impl.workspace_.readPreferences()->value.selected_layout == stored_id);
    const auto preferences_file = root / ".lux/workspace/preferences.toml";
    const auto preferences_before = storage::readPublicationFile(preferences_file, 65536);
    assert(preferences_before);
    {
        std::ofstream output(preferences_file, std::ios::binary | std::ios::trunc);
        output << "bad = [";
    }
    assert(impl.desktop_->views().show(views->front().id));
    auto applied_with_bad_preferences = impl.executeWorkspaceIntent(lux::editor::project::ApplyLayout{stored_id});
    assert(
        !applied_with_bad_preferences &&
        applied_with_bad_preferences.error().domain == "workspace.applied.preferences-read"
    );
    assert(!impl.desktop_->views().describe(views->front().id)->visible); // UI commit is not rolled back.
    {
        std::ofstream output(preferences_file, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(preferences_before->data()), preferences_before->size());
    }
    assert(impl.executeWorkspaceIntent(lux::editor::project::RemoveLayout{stored_id}));
    settle_workspace();
    assert(impl.workspace_changes_.catalog().layouts.empty());
    for (auto ticket : [&] {
             std::vector<persistence::WriteTicket> ids;
             for (const auto& report : impl.workspace_changes_.publications())
                 ids.push_back(report.ticket);
             return ids;
         }())
        assert(impl.executeWorkspaceIntent(lux::editor::project::AcknowledgeWorkspace{ticket}));
    assert(impl.workspace_changes_.publications().empty());
    std::cout << "Workspace UI uses stable layout IDs and one publication coordinator; preference failure preserves UI "
                 "commit\n";
    const auto create_content = [&](const char* command) {
        auto accepted = app->execute(commands::CommandId{command});
        assert(accepted);
        auto operation = std::get<commands::AcceptedOperation>(*accepted);
        assert(operation.kind == commands::OperationKindId{"open"});
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
    const auto material_view = contentViews(impl.desktop_->views(), material_id).front().id;
    auto material_action = [&](auto action) {
        auto invoke = [&](ui::Pane& pane) { action(static_cast<lux::editor::material::MaterialView&>(pane)); };
        assert(impl.desktop_->views().withView(material_view, invoke));
    };
    material_action([&](lux::editor::material::MaterialView& view) {
        assert(view.beginEdit("Create output"));
        std::vector<lux::editor::material::VMaterialEdit> edits;
        edits.emplace_back(lux::editor::material::MaterialInsertNode{
            contracts::CodeLease::builtin(),
            std::make_unique<lux::material::OutputSurfaceNode>()
        });
        assert(view.previewEdit(edits) && view.commitEdit());
    });
    const auto save_material_stamp = impl.sessions_.describe(material_id)->current;
    assert(app->execute(
        commands::CommandId{"lux.editor.save-as"},
        commands::CommandInvocation{commands::SessionTarget{material_id, save_material_stamp}}
    ));
    assert(impl.save_question_);
    const auto original_save_target = impl.save_question_->target;
    auto invalid_path = [&](ui::Pane& pane) {
        auto& question = static_cast<desktop::ReviewView&>(pane);
        assert(question.setText("../outside.source"));
        assert(question.answer(desktop::EReviewChoice::SAVE));
    };
    assert(impl.desktop_->views().withView(impl.save_question_->view, invalid_path));
    assert(app->update() && impl.save_question_ && impl.content_saving_->pending().empty());
    assert(impl.save_question_->target.based_on == original_save_target.based_on);
    auto choose_source = [&](ui::Pane& pane) {
        auto& question = static_cast<desktop::ReviewView&>(pane);
        assert(question.setText("Content/Beginner/Material.source"));
        assert(question.answer(desktop::EReviewChoice::SAVE));
    };
    assert(impl.desktop_->views().withView(impl.save_question_->view, choose_source));
    assert(app->update());
    const auto save_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!impl.content_saving_->pending().empty())
    {
        assert(std::chrono::steady_clock::now() < save_deadline);
        auto frame = app->update();
        if (!frame)
            std::cerr << "save frame: " << frame.error().domain << '\n';
        assert(frame);
        std::this_thread::yield();
    }
    assert(!impl.content_saving_->reports().empty() && !impl.content_saving_->reports().back().failure && impl.content_saving_->reports().back().result);
    const auto saved_material = impl.sessions_.describe(material_id);
    assert(saved_material && !saved_material->dirty && saved_material->current == save_material_stamp);
    assert(saved_material->binding && impl.project_->asset(saved_material->binding->asset));
    assert(std::filesystem::exists(impl.project_->root() / "Content/Beginner/Material.source"));
    const auto reopened = app->open(impl.project_->reference(saved_material->binding->asset));
    assert(reopened);
    assert(app->update());
    const auto reopened_status = app->openStatus(*reopened);
    assert(reopened_status && reopened_status->content.session == material_id);
    assert(app->acknowledgeOpen(*reopened));
    std::cout << "Real Save As preserves author history, publishes source/catalog and reopens the same Session\n";
    // Layouts prepare unbound windows. Only the independent recovery manifest opens author content.
    auto unbound_capture = impl.executeWorkspaceIntent(lux::editor::project::CaptureRecovery{});
    assert(!unbound_capture && unbound_capture.error().domain == "recovery.unbound");
    assert(!impl.workspace_.readRecovery()); // Unsaved Flow prevented any partial manifest publication.
    const auto material_window = *impl.desktop_->views().describe(material_view);
    auto current_layout = impl.desktop_->views().captureLayout(layout_id, "Recovery qualification");
    assert(current_layout);
    const auto material_slot =
        std::ranges::find(current_layout->slots, material_window.restore_key, &workspace::LayoutSlot::restore_key);
    assert(material_slot != current_layout->slots.end());
    workspace::DockLayout recovery_layout;
    recovery_layout.id = layout_id;
    recovery_layout.label = "Unbound recovery window";
    recovery_layout.slots.push_back(*material_slot);
    recovery_layout.slots.front().id = {1};
    recovery_layout.slots.front().restore_key = views::ViewRestoreKey{"recovery-material"};
    recovery_layout.dock.nodes.push_back({1, workspace::EDockSplit::LEAF, 0, 0, 0.5, {{1}}});
    recovery_layout.dock.roots.push_back({1});
    assert(app->applyLayout(recovery_layout));
    auto recovery_windows = impl.desktop_->views().describeAll();
    assert(recovery_windows);
    auto unbound_window =
        std::ranges::find(*recovery_windows, views::ViewRestoreKey{"recovery-material"}, &views::ViewInfo::restore_key);
    assert(unbound_window != recovery_windows->end());
    const auto recovered_view = unbound_window->id;
    lux::editor::material::MaterialViewState camera_before;
    auto read_camera = [&](ui::Pane& pane) {
        auto& material = static_cast<lux::editor::material::MaterialView&>(pane);
        assert(!material.binding());
        camera_before = material.state();
    };
    assert(impl.desktop_->views().withView(recovered_view, read_camera));
    workspace::RecoveryManifest recovery_manifest;
    const auto locator = "asset:" + uuids::to_string(saved_material->binding->asset.uuid());
    recovery_manifest.entries = {
        {material_window.restore_key, material_window.type, {{locator, true}}, 0},
        {unbound_window->restore_key, unbound_window->type, {{locator, false}}, 0},
        {views::ViewRestoreKey{"future-window"}, views::ViewTypeId{"future.provider"}, {{"future:opaque", false}}, 0}
    };
    recovery_manifest.opaque.push_back({"future-data", 4, {std::byte{5}, std::byte{9}}});
    assert(impl.workspace_changes_.recordRecovery(recovery_manifest, "missing"));
    settle_workspace();
    auto recovery_before = workspace::encodeRecovery(impl.workspace_.readRecovery()->value);
    assert(recovery_before);
    assert(impl.executeWorkspaceIntent(lux::editor::project::RestoreRecovery{}));
    auto while_catalog_busy = [&](const extensions::ContributionSnapshot&) -> extensions::ContributionResult<void> {
        assert(impl.settleRecovery());
        assert(!impl.restoration_->items().front().opening && !impl.restoration_->items().front().result);
        return {};
    };
    assert(impl.contributions_.withSnapshot(while_catalog_busy));
    const auto recovery_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::ranges::any_of(impl.restoration_->items(), [](const auto& entry) { return !entry.result; })
    )
    {
        assert(std::chrono::steady_clock::now() < recovery_deadline);
        assert(app->update());
    }
    assert(impl.restoration_->items()[0].result->has_value() && **impl.restoration_->items()[0].result == material_view);
    assert(impl.restoration_->items()[1].result->has_value() && **impl.restoration_->items()[1].result == recovered_view);
    assert(impl.restoration_->items()[2].result && !*impl.restoration_->items()[2].result);
    assert(impl.sessions_.size() == 2 && impl.sessions_.describe(material_id)->current == saved_material->current);
    assert(impl.desktop_->views().describeAll()->size() == recovery_windows->size());
    auto check_recovery = [&](ui::Pane& pane) {
        auto& material = static_cast<lux::editor::material::MaterialView&>(pane);
        assert(material.binding() && material.binding()->session.id() == material_id);
        assert(material.state().camera.transform.translation.isApprox(camera_before.camera.transform.translation));
    };
    assert(impl.desktop_->views().withView(recovered_view, check_recovery));
    assert(*workspace::encodeRecovery(impl.workspace_.readRecovery()->value) == *recovery_before);
    assert(app->closeView(recovered_view));
    for (int i = 0; i < 10 && impl.desktop_->views().describe(recovered_view); ++i)
        assert(app->update());
    assert(!impl.desktop_->views().describe(recovered_view));
    std::cout << "Recovery uses an independent immutable manifest, keeps BUSY input, reuses exact windows and "
                 "preserves unknown bytes/camera\n";
    lux::editor::material::MaterialCompileId compilation;
    material_action([&](lux::editor::material::MaterialView& view) {
        auto result = view.compile();
        assert(result);
        compilation = *result;
    });
    const auto compile_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!impl.material_compilation_.operation(compilation)->get().ready())
    {
        assert(std::chrono::steady_clock::now() < compile_deadline);
        assert(app->update());
    }
    assert(impl.material_compilation_.operation(compilation)->get().result());
    auto rename_material = [&] {
        material_action([&](lux::editor::material::MaterialView& view) {
            assert(view.beginEdit("Edit after compile"));
            std::vector<lux::editor::material::VMaterialEdit> edits;
            edits.emplace_back(lux::editor::material::MaterialRename{"Newer author source"});
            assert(view.previewEdit(edits) && view.commitEdit());
        });
    };
    material_action([&](lux::editor::material::MaterialView& view) { assert(view.requestPublication()); });
    rename_material();
    assert(app->update());
    assert(impl.artifacts_.back().terminal() && impl.artifacts_.back().failure && !impl.artifacts_.back().operation);
    material_action([&](lux::editor::material::MaterialView& view) { assert(view.undo() && view.requestPublication()); }
    );
    assert(app->update() && impl.artifacts_.back().operation && !impl.artifacts_.back().pending);
    rename_material(); // Already admitted work owns the older capture, not this live source.
    const auto newer_material = impl.sessions_.describe(material_id);
    while (!impl.artifacts_.back().terminal())
    {
        assert(std::chrono::steady_clock::now() < compile_deadline);
        assert(app->update());
    }
    const auto& published_material = impl.artifacts_.back();
    if (published_material.failure)
        std::cerr << published_material.failure->domain << ": " << published_material.failure->message << '\n';
    assert(!published_material.failure && published_material.operation);
    assert(std::holds_alternative<PublicationSucceeded>(published_material.operation->status()));
    auto* compiled_entry = impl.project_->asset(saved_material->binding->asset);
    assert(compiled_entry && !compiled_entry->cooked_path.empty());
    assert(compiled_entry->compiled_source_digest == compiled_entry->source_digest);
    assert(impl.project_->catalogAsset(compiled_entry->id));
    assert(std::filesystem::exists(root / compiled_entry->cooked_path));
    auto current_material = impl.sessions_.describe(material_id);
    assert(current_material->current == newer_material->current && current_material->dirty == newer_material->dirty);
    material_action([&](lux::editor::material::MaterialView& view) { assert(view.undo()); });
    assert(impl.sessions_.describe(material_id)->current == saved_material->current);
    std::cout
        << "Real compile intent publishes a readable versioned package; stale intent rejected and baseline unchanged\n";

    assert(app->execute(
        commands::CommandId{"lux.editor.export-copy"},
        commands::CommandInvocation{commands::SessionTarget{material_id, saved_material->current}}
    ));
    auto export_path = [&](ui::Pane& pane) {
        auto& question = static_cast<desktop::ReviewView&>(pane);
        assert(question.setText("Content/Beginner/MaterialCopy.source"));
        assert(question.answer(desktop::EReviewChoice::SAVE));
    };
    assert(impl.desktop_->views().withView(impl.save_question_->view, export_path));
    assert(app->update());
    while (!impl.content_saving_->pending().empty())
    {
        assert(std::chrono::steady_clock::now() < save_deadline);
        assert(app->update());
        std::this_thread::yield();
    }
    bool preview_visible{};
    const auto preview_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!preview_visible)
    {
        if (std::chrono::steady_clock::now() >= preview_deadline)
        {
            material_action([&](lux::editor::material::MaterialView& view) {
                const auto state = view.previewStatus();
                std::cerr << "Preview: prepared=" << bool(state.prepared) << " accepted=" << bool(state.accepted)
                          << " stale=" << state.stale << " diagnostic=" << state.diagnostic << '\n';
            });
            assert(false && "Actual material preview did not become visible");
        }
        assert(app->update());
        material_action([&](lux::editor::material::MaterialView& view) { preview_visible = view.image().isValid(); });
    }
    const auto after_copy = impl.sessions_.describe(material_id);
    assert(
        after_copy && after_copy->binding == saved_material->binding && after_copy->current == saved_material->current
    );
    assert(!after_copy->dirty && std::filesystem::exists(root / "Content/Beginner/MaterialCopy.source"));
    assert(app->execute(
        commands::CommandId{"lux.editor.reload"},
        commands::CommandInvocation{commands::SessionTarget{material_id, after_copy->current}}
    ));
    while (!impl.reloads_.back().result)
    {
        assert(std::chrono::steady_clock::now() < save_deadline);
        assert(app->update());
        std::this_thread::yield();
    }
    assert(*impl.reloads_.back().result);
    const auto reloaded_material = impl.sessions_.describe(material_id);
    assert(reloaded_material && reloaded_material->current != after_copy->current && !reloaded_material->dirty);
    assert(reloaded_material->binding == after_copy->binding);
    std::cout << "Export Copy leaves the baseline intact; actual Reload keeps SessionId and replaces history\n";
    assert(app->execute(commands::CommandId{"lux.editor.content.results"}));
    assert(app->update()); // Draw the installed result view, including save/reload and unbound content rows.

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
    for (const auto& view : contentViews(impl.desktop_->views(), material_id))
        material_views.push_back(view.id);
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
    const auto flow_views = contentViews(impl.desktop_->views(), flow_id);
    assert(!flow_views.empty());
    const auto flow_view_id = flow_views.front().id;
    assert(app->closeView(flow_view_id));
    choose_last(desktop::EReviewChoice::CLOSE_CONTENT);
    assert(impl.review_ && impl.sessions_.size() == 2);
    auto choose_flow_source = [&](ui::Pane& pane) {
        assert(static_cast<desktop::ReviewView&>(pane).setText("Content/Beginner/Flow.source"));
    };
    assert(impl.desktop_->views().withView(*impl.review_, choose_flow_source));
    answer(desktop::EReviewChoice::SAVE);
    const auto close_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    do
    {
        assert(std::chrono::steady_clock::now() < close_deadline);
        assert(app->update());
        assert(!impl.exit_failure_);
        std::this_thread::yield();
    } while (app->phase() != EApplicationPhase::RUNNING || !impl.content_saving_->pending().empty());
    assert(std::filesystem::exists(impl.project_->root() / "Content/Beginner/Flow.source"));
    assert(!impl.sessions_.describe(flow_id) && !impl.desktop_->views().describe(flow_view_id));
    assert(impl.sessions_.describe(material_id)->current == material_before);
    std::cout << "Last-view Cancel/Keep/Close use real modals; closing one content preserves another\n";

    // Composite recovery reads two real, previously saved author sources. The window factory only
    // binds their identities; there is no application branch for this comparison view.
    {
        auto previous_catalog = impl.contributions_.snapshot();
        extensions::ContributionDraft draft;
        const auto copy = [](auto& destination, auto values) { destination.assign(values.begin(), values.end()); };
        copy(draft.commands, previous_catalog.commands().entries());
        copy(draft.sessions, previous_catalog.sessions().entries());
        copy(draft.views, previous_catalog.views().entries());
        copy(draft.components, previous_catalog.components());
        copy(draft.configurations, previous_catalog.configurations());
        bool refuse_view{};
        draft.views.push_back(views::ViewFactoryEntry::create(
            contracts::CodeLease::builtin(),
            views::ViewFactoryDescriptor{
                views::ViewTypeIdView{"test.comparison"}, "Comparison", cxx::typeToken<views::ContentViewInput>(), 1,
                std::array{sessions::SessionKindIdView{"lux.editor.material"},
                           sessions::SessionKindIdView{"lux.editor.flowforge"}}, false
            },
            [&](const views::ViewFactoryInput& input) -> views::ViewFactoryResult<views::DetachedView> {
                const auto& binding = *static_cast<const views::ContentViewInput*>(input.binding());
                assert(binding.content.sessions.size() == 2 && binding.content.primary == binding.content.sessions[1]);
                for (const auto session : binding.content.sessions)
                    assert(impl.sessions_.describe(session));
                if (refuse_view)
                    return cxx::unexpected(views::ViewFactoryFailure{
                        views::EViewFactoryError::CONSTRUCT, "comparison.deliberate"
                    });
                return views::DetachedView{
                    contracts::CodeLease::builtin(),
                    std::make_unique<ComparisonPane>(input.dispatcher(), input.paneId(), binding.content),
                    nullptr, nullptr, nullptr, nullptr,
                    +[](const ui::Pane& pane) noexcept { return static_cast<const ComparisonPane&>(pane).content(); }
                };
            }
        ));
        auto extended = extensions::ContributionSnapshot::prepare(std::move(draft));
        assert(extended && impl.contributions_.enqueue(*extended) && impl.contributions_.applyPending());
        for (const auto& report : impl.content_saving_->reports())
            assert(report.result && !report.failure);
        const auto& assets = impl.project_->manifest().assets;
        const auto flow_asset = std::ranges::find(assets, std::string{"Content/Beginner/Flow.source"},
                                                &ProjectAssetEntry::source_path);
        if (flow_asset == assets.end())
            for (const auto& report : impl.content_saving_->reports())
                std::cerr << "save source=" << report.asset.source_path << " type=" << report.asset.source_type
                          << " failure=" << (report.failure ? report.failure->domain + ":" + report.failure->message : "none")
                          << " result=" << bool(report.result) << '\n';
        assert(flow_asset != assets.end());
        workspace::RecoveryManifest composite;
        composite.entries.push_back({
            views::ViewRestoreKey{"comparison"}, views::ViewTypeId{"test.comparison"},
            {{locator, false}, {"asset:" + uuids::to_string(flow_asset->id.uuid()), false}}, 1
        });
        const auto publish = [&] {
            const auto stored = impl.workspace_.readRecovery();
            assert(stored);
            assert(impl.workspace_changes_.recordRecovery(composite, stored->target.expected_version));
            settle_workspace();
        };
        publish();
        const auto recover = [&] {
            assert(impl.restoreRecovery());
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
            while (!impl.restoration_->items().front().result)
            {
                assert(std::chrono::steady_clock::now() < deadline);
                assert(app->update());
            }
        };
        recover();
        const auto& first_result = impl.restoration_->items().front();
        assert(first_result.sources.size() == 2 && *first_result.result);
        const auto comparison = **first_result.result;
        const auto restored_flow = first_result.sources[1].session;
        assert(restored_flow != flow_id && impl.sessions_.describe(restored_flow));
        const auto associated = impl.desktop_->views().describe(comparison);
        assert(associated && associated->content.sessions ==
            (std::vector<sessions::SessionId>{material_id, restored_flow}));
        assert(associated->content.primary == restored_flow);
        assert(impl.captureRecovery());
        settle_workspace();
        const auto captured = impl.workspace_.readRecovery();
        const auto saved = std::ranges::find(captured->value.entries, views::ViewRestoreKey{"comparison"},
                                            &workspace::RecoveryEntry::restore_key);
        assert(saved != captured->value.entries.end() && saved->contents.size() == 2 && saved->primary == 1);
        assert(saved->contents[0].locator == locator && saved->contents[1].locator == composite.entries[0].contents[1].locator);
        assert(impl.desktop_->views().close(comparison) && impl.desktop_->views().drain());
        assert(!impl.desktop_->views().describe(comparison));
        assert(impl.sessions_.describe(material_id) && impl.sessions_.describe(restored_flow));
        publish();
        refuse_view = true;
        recover();
        const auto& rejected = impl.restoration_->items().front();
        assert(rejected.sources.size() == 2 && rejected.result && !*rejected.result);
        for (const auto& source : rejected.sources)
            assert(source.stage == sessions::EOpenAssetStage::PUBLISHED && impl.sessions_.describe(source.session));
        assert(impl.contributions_.enqueue(previous_catalog) && impl.contributions_.applyPending());
        assert(impl.requestClose(impl.sessions_.describe(restored_flow)->current));
        while (app->phase() != EApplicationPhase::RUNNING)
            assert(app->update());
        assert(!impl.sessions_.describe(restored_flow) && impl.sessions_.describe(material_id));
        std::cout << "EC1 composite recovery: all sources/primary retained, real IO, failed view preserves content\n";
    }

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
        const auto entries = impl.desktop_->views().describeAll();
        assert(entries);
        for (const auto& record : *entries)
            if (record.type == views::ViewTypeId{"lux.editor.scene.view"} && record.content.primary)
                scene_id = *record.content.primary;
    }
    const auto scene_records = contentViews(impl.desktop_->views(), scene_id);
    assert(!scene_records.empty());
    const auto scene_view = scene_records.front().id;
    const auto model_source = impl.sessions_.describe(scene_id)->current;
    for (int frame = 0; frame < 32; ++frame)
        assert(app->update());
    assert(app->execute(commands::CommandId{"lux.editor.scene.resources"}, commands::CommandInvocation{scene_view}));
    auto resource_views = impl.desktop_->views().describeAll();
    assert(resource_views);
    auto resources = std::ranges::find_if(*resource_views, [](const auto& view) {
        return view.type == views::ViewTypeId{"lux.editor.resources"};
    });
    assert(resources != resource_views->end());
    auto drop_model = [&](ui::Pane& pane) {
        auto requested = static_cast<lux::editor::scene::SceneView&>(pane)
                             .dropModel(impl.project_->catalogModel().reference(model_id), {320, 420}, {640, 480});
        if (!requested)
            std::cerr << "Model drop preparation failed, variant " << requested.error().cause.index() << '\n';
        assert(requested);
    };
    assert(impl.desktop_->views().withView(scene_view, drop_model));
    assert(impl.model_placements_.size() == 1 && !impl.model_placements_.front().operation);
    assert(impl.model_placements_.front().placement.based_on == model_source);
    const auto model_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!impl.model_placements_.front().result)
    {
        assert(std::chrono::steady_clock::now() < model_deadline);
        assert(app->update());
        assert(!impl.model_placements_.front().failure);
        std::this_thread::yield();
    }
    const auto& model_result = *impl.model_placements_.front().result;
    if (!model_result)
        std::cerr << "Model insertion failed, variant " << model_result.error().cause.index() << '\n';
    assert(model_result && !impl.model_placements_.front().operation);
    assert(impl.sessions_.describe(scene_id)->current != model_source);
    auto author_key = impl.sessions_.key<lux::editor::scene::SceneSession>(scene_id);
    assert(author_key);
    {
        auto author = impl.sessions_.access<lux::editor::scene::SceneSession>().edit(*author_key);
        assert(author);
        assert(author->get().capture()->objects().size() == 2);
        assert(author->get().undo());
        assert(author->get().describe().current == model_source);
        assert(author->get().redo());
    }
    impl.result_intent_ = lux::editor::project::AcknowledgeModel{
        impl.model_placements_.front().id
    };
    assert(app->update() && impl.model_placements_.empty());
    std::cout << "Actual SceneView drop reads project pak through Process and commits one undoable model batch\n";
    for (int frame = 0; frame < 8; ++frame)
        assert(app->update());
    lux::scene::SceneInstanceId presented;
    auto read_instance = [&](ui::Pane& pane) {
        presented = static_cast<lux::editor::scene::SceneView&>(pane).presentedInstance();
    };
    assert(impl.desktop_->views().withView(scene_view, read_instance));
    assert(presented.valid());
    auto read_resources = [&](ui::Pane& pane) {
        assert(static_cast<lux::editor::scene::ResourceView&>(pane).snapshot().instance == presented);
    };
    assert(impl.desktop_->views().withView(resources->id, read_resources));

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
    auto shared_interaction = lux::editor::scene::shareSceneInteraction(impl.desktop_->views(), scene_view);
    assert(shared_interaction);
    auto interaction = *shared_interaction;
    assert(interaction->select(
        {{lux::editor::scene::SceneObjectRef{scene_id, impl.sessions_.describe(scene_id)->current.state.history, object}
        }}
    ));
    assert(app->execute(commands::CommandId{"lux.editor.scene.inspector"}, commands::CommandInvocation{scene_view}));
    auto inspector_views = impl.desktop_->views().describeAll();
    auto inspector_info = std::ranges::find_if(*inspector_views, [](const auto& view) {
        return view.type == views::ViewTypeId{"lux.editor.inspector"};
    });
    assert(inspector_info != inspector_views->end());
    assert(interaction->select({}));
    assert(app->update());
    auto no_target = [&](ui::Pane& pane) { assert(!static_cast<lux::editor::scene::InspectorView&>(pane).target()); };
    assert(impl.desktop_->views().withView(inspector_info->id, no_target));

    // An unrelated real SceneSystem failure must not bypass the accepted source-save/catalog handoff.
    auto saving = impl.save({material_id, impl.sessions_.describe(material_id)->current}, persistence::ESaveMode::SAVE);
    assert(saving);
    const auto saving_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (impl.saves_.status(*saving)->stage != persistence::ESaveStage::TERMINAL)
    {
        assert(std::chrono::steady_clock::now() < saving_deadline);
        assert(impl.engine_->execution().collectCompletions());
        impl.saves_.adoptCompletions();
        assert(impl.save_execution_.submitReady());
        std::this_thread::yield();
    }
    auto registration = failureRegistration();
    lux::scene::SceneDescriptionBuilder failure_builder;
    assert(failure_builder.addSystem({1}, "failure", registration.type, 1, {}, 0));
    auto failure_description = std::move(failure_builder).buildResolved();
    assert(failure_description);
    auto snapshot = impl.sessions_.access<lux::editor::scene::SceneSession>().read(*author_key)->get().capture();
    assert(snapshot);
    auto failing =
        impl.engine_->sceneRuntime()
            .builder()
            .setDescription(std::make_shared<const lux::scene::SceneDescription>(std::move(*failure_description)))
            .setWorld(std::shared_ptr<const world::WorldDescription>(
                snapshot->configuration().world,
                &snapshot->configuration().world->data()
            ))
            .setSimulation(std::shared_ptr<const simulation::SimulationDescription>(
                snapshot->configuration().simulation,
                &snapshot->configuration().simulation->data()
            ))
            .setRegistrations(
                impl.registrations_.components,
                *impl.registrations_.simulation_systems,
                std::span{&registration, 1}
            )
            .build();
    assert(failing);
    auto failed_frame = app->update();
    assert(!failed_frame && failed_frame.error().domain == "scene.execution");
    auto failed_save =
        std::ranges::find(impl.content_saving_->reports(), *saving, &ProjectSaveReport::id);
    assert(failed_save != impl.content_saving_->reports().end() && failed_save->catalog_ticket);
    using Failures = std::remove_reference_t<decltype(impl)>::SceneFailures;
    auto retained = std::any_cast<std::shared_ptr<const Failures>>(failed_frame.error().cause);
    assert(retained && retained->values.size() == 1);
    const auto failed_instance = retained->values.front().scene;
    auto retired_failure = failing->retire();
    while (!retired_failure.complete() || std::ranges::find(impl.content_saving_->pending(), *saving) != impl.content_saving_->pending().end())
    {
        assert(std::chrono::steady_clock::now() < saving_deadline);
        auto updated = app->update();
        if (!updated)
        {
            assert(updated.error().domain == "scene.execution");
            auto retiring = std::any_cast<std::shared_ptr<const Failures>>(updated.error().cause);
            assert(retiring && retiring->values.size() == 1);
            assert(retiring->values.front().scene == failed_instance);
            const auto& original = std::get<lux::scene::SceneDriveFailure>(retiring->values.front().cause);
            assert(std::any_cast<int>(std::get<lux::scene::SceneExecutionFailure>(original.cause).cause) == 731);
        }
        std::this_thread::yield();
    }
    const auto& drive_failure = std::get<lux::scene::SceneDriveFailure>(retained->values.front().cause);
    assert(retained->values.front().scene == failed_instance);
    assert(std::any_cast<int>(std::get<lux::scene::SceneExecutionFailure>(drive_failure.cause).cause) == 731);
    impl.result_intent_ = lux::editor::project::AcknowledgeMaintenance{
    };
    assert(app->update() && !impl.maintenance_failure_);
    std::cout << "Scene failure keeps an owning diagnostic and independent save completion; empty selection clears "
                 "Inspector\n";
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
    assert(!impl.run_presentations_.front().views.empty());
    const auto run_view = impl.run_presentations_.front().views.front();
    assert(app->execute(commands::CommandId{"lux.editor.scene.pause"}, commands::CommandInvocation{run_view}));
    while (impl.runs_.info(run)->pause_pending)
    {
        assert(app->update());
    }
    assert(app->execute(commands::CommandId{"lux.editor.scene.step"}, commands::CommandInvocation{run_view}));
    const auto step = impl.run_presentations_.front().steps.front();
    const auto step_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (impl.runs_.info(run)->pause_pending)
    {
        assert(std::chrono::steady_clock::now() < step_deadline);
        assert(app->update());
        std::this_thread::yield();
    }
    assert(impl.runs_.stepStatus(step)); // Completion has not been silently acknowledged by an update.
    assert(impl.runs_.stepStatus(step)->state == lux::scene::ESceneStepState::COMPLETED);
    assert(impl.requestClose(impl.sessions_.describe(scene_id)->current));
    assert(app->update() && impl.review_);
    answer(desktop::EReviewChoice::DISCARD);
    assert(app->update() && impl.review_ && impl.review_run_ == run);
    answer(desktop::EReviewChoice::KEEP_RUN);
    assert(app->update() && app->phase() == EApplicationPhase::RUNNING);
    assert(!impl.sessions_.describe(scene_id) && impl.runs_.info(run));
    assert(impl.desktop_->views().describe(run_view));
    auto running_interaction = lux::editor::scene::shareSceneInteraction(impl.desktop_->views(), run_view);
    assert(running_interaction && (*running_interaction)->synchronize());
    assert(app->execute(commands::CommandId{"lux.editor.scene.outliner"}, commands::CommandInvocation{run_view}));
    assert(app->execute(commands::CommandId{"lux.editor.scene.resume"}, commands::CommandInvocation{run_view}));
    for (int frame = 0; frame < 4; ++frame)
        assert(app->update());
    assert(impl.runs_.info(run)->provenance.content == run_source);
    std::cout << "Formal scene form, Outliner, frozen Play/Pause/Step/Resume and Keep Run after author close\n";

    // Formal factories, concrete UI admission and application-owned completion survive the window.
    assert(app->execute(commands::CommandId{"lux.editor.import"}));
    auto tool_views = impl.desktop_->views().describeAll();
    assert(tool_views);
    const auto import_view =
        std::ranges::find(*tool_views, views::ViewTypeId{"lux.editor.import"}, &views::ViewInfo::type)->id;
    const auto imported_id = asset::AssetId{*uuids::uuid::from_string("091adbc2-cc75-42c0-b33a-a5f4e4fb8e07")};
    const auto import_file = root / "import-ui.obj";
    {
        std::ofstream file(import_file);
        file << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    }
    std::optional<assets::ModelImportId> importing;
    auto request_import = [&](ui::Pane& pane) {
        auto accepted = static_cast<lux::editor::project::ImportView&>(pane).importModel(
            {imported_id, std::filesystem::absolute(import_file), "Content/Imported/Triangle", {}}
        );
        assert(accepted);
        importing = *accepted;
    };
    assert(impl.desktop_->views().withView(import_view, request_import));
    assert(app->closeView(import_view));
    const auto import_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!std::holds_alternative<assets::ModelImportSucceeded>(*impl.importer_->status(*importing)))
    {
        assert(std::chrono::steady_clock::now() < import_deadline);
        assert(app->update());
        auto status = impl.importer_->status(*importing);
        if (const auto* error = std::get_if<EditorFailure>(&*status))
            std::cerr << error->domain << '\n';
        assert(!std::holds_alternative<EditorFailure>(*status));
        std::this_thread::yield();
    }
    assert(impl.project_->asset(imported_id) && !impl.desktop_->views().describe(import_view));
    assert(impl.importer_->acknowledge(*importing));
    assert(app->execute(commands::CommandId{"lux.editor.project.create"}));
    auto creation_requests = impl.project_creation_->requests();
    const auto creation_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    auto settle_creation = [&](ProjectCreation& activity) {
        while (activity.progress().pending)
        {
            assert(std::chrono::steady_clock::now() < creation_deadline);
            assert(app->update());
            activity.update();
            std::this_thread::yield();
        }
    };
    settle_creation(*impl.project_creation_);
    assert(creation_requests.catalog() && !impl.project_creation_->progress().failure);
    assert(creation_requests.select({}));
    settle_creation(*impl.project_creation_);
    assert(!impl.project_creation_->progress().failure);
    const auto minimal = root / "MinimalProject";
    assert(creation_requests.create({std::filesystem::absolute(minimal), "Minimal", "", {}}));
    tool_views = impl.desktop_->views().describeAll();
    const auto creation_view =
        std::ranges::find(*tool_views, views::ViewTypeId{"lux.editor.project.creation"}, &views::ViewInfo::type)->id;
    assert(app->closeView(creation_view));
    settle_creation(*impl.project_creation_);
    assert(impl.project_creation_->progress().committed && !impl.project_creation_->progress().failure);
    assert(std::filesystem::exists(minimal / "Project.luxproject") && !std::filesystem::exists(minimal / "Content"));
    assert(prepareProjectOpen(minimal / "Project.luxproject"));
    for (const auto preset :
         {editor::scene::ESceneContentPreset::TWO_DIMENSIONAL, editor::scene::ESceneContentPreset::THREE_DIMENSIONAL})
    {
        ProjectCreation creation(impl.engine_->execution(), impl.messages_.dispatcherRef(), argv[1], false);
        auto requests = creation.requests();
        assert(creation.start());
        settle_creation(creation);
        std::vector<ProjectPluginEntry> selection;
        for (const auto& plugin : requests.catalog()->plugins())
            if (plugin.builtin)
                selection.push_back({plugin.identity.id, plugin.identity.version});
        assert(requests.select(std::move(selection)));
        settle_creation(creation);
        if (creation.progress().failure)
            std::cerr << creation.progress().failure->domain << '\n';
        assert(!creation.progress().failure);
        auto inputs = requests.configuration();
        assert(inputs);
        editor::scene::SceneConfigurationResult<void> configured;
        ui::Pane form_pane(
            impl.messages_.dispatcherRef(),
            ui::PaneId{"creation-test"},
            ui::PaneTypeId{"creation-test"},
            "Creation"
        );
        ui::Layout fields(form_pane, ui::ElementId{"fields"});
        editor::scene::SceneConfigurationElement
            form(fields, ui::ElementId{"configuration"}, inputs->scene, configured);
        assert(configured && form.applyPreset(preset));
        auto description = form.build();
        assert(description);
        const auto destination =
            root / (preset == editor::scene::ESceneContentPreset::TWO_DIMENSIONAL ? "Initial2D" : "Initial3D");
        assert(requests.create(
            {std::filesystem::absolute(destination), "Beginner", "Beginner", std::move(*description), inputs->plugins}
        ));
        settle_creation(creation);
        if (creation.progress().failure)
            std::cerr << creation.progress().failure->domain << '\n';
        assert(creation.progress().committed && !creation.progress().failure);
        assert(prepareProjectOpen(destination / "Project.luxproject"));
        assert(std::filesystem::exists(destination / "Content/Beginner/Main.scene"));
    }
    // Failed creation of an existing directory never overwrites its manifest or adopts a false commit.
    ProjectCreation rejected_creation(impl.engine_->execution(), impl.messages_.dispatcherRef(), argv[1], false);
    auto rejected_requests = rejected_creation.requests();
    assert(rejected_requests.select({}));
    settle_creation(rejected_creation);
    const auto original_manifest = prepareProjectOpen(minimal / "Project.luxproject");
    assert(rejected_requests.create({std::filesystem::absolute(minimal), "Overwrite", "", {}}));
    settle_creation(rejected_creation);
    assert(rejected_creation.progress().failure && !rejected_creation.progress().committed);
    assert(prepareProjectOpen(minimal / "Project.luxproject")->manifest().name == original_manifest->manifest().name);
    std::cout
        << "Formal project creation: selected V7 plugins, minimal/2D/3D real files, closed view, conflict retained\n";

    assert(app->execute(commands::CommandId{"lux.editor.settings"}));
    tool_views = impl.desktop_->views().describeAll();
    const auto settings_view =
        std::ranges::find(*tool_views, views::ViewTypeId{"lux.editor.settings"}, &views::ViewInfo::type)->id;
    const auto active_plugins = impl.plugins_.libraries().size();
    auto choose_plugins = [&](ui::Pane& pane) {
        assert(static_cast<lux::editor::project::SettingsView&>(pane).requestSave({}));
    };
    assert(impl.desktop_->views().withView(settings_view, choose_plugins));
    assert(app->update() && impl.plugin_saving_->status());
    assert(app->closeView(settings_view));
    while (!impl.plugin_saving_->settled())
    {
        assert(std::chrono::steady_clock::now() < import_deadline);
        assert(app->update());
        assert(!std::holds_alternative<EditorFailure>(*impl.plugin_saving_->status()));
        std::this_thread::yield();
    }
    assert(impl.project_->manifest().plugins.empty() && impl.plugins_.libraries().size() == active_plugins);
    assert(!impl.desktop_->views().describe(settings_view));
    std::cout << "Formal import/settings factories: UI closes, immutable import and plugin publication finish; active "
                 "code stays pinned\n";

    // Accepted source encoding and real material compilation remain owned while the Run/GPU are live.
    const auto exit_content = impl.sessions_.describe(material_id)->current;
    auto exit_save = impl.save({material_id, exit_content}, persistence::ESaveMode::SAVE);
    assert(exit_save && impl.saves_.status(*exit_save)->stage != persistence::ESaveStage::TERMINAL);
    lux::editor::material::MaterialCompileId exit_compile;
    auto compile_at_exit = [&](ui::Pane& pane) {
        auto compiled = static_cast<lux::editor::material::MaterialView&>(pane).compile();
        assert(compiled);
        exit_compile = *compiled;
    };
    assert(impl.desktop_->views().withView(*shown_again, compile_at_exit));
    auto exit_operation = impl.material_compilation_.operation(exit_compile);
    assert(exit_operation && !exit_operation->get().ready());
    const auto exit_task = exit_operation->get().task();
    assert(impl.runs_.info(run) && !impl.engine_->renderContext()->resources().empty());
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
    assert(impl.material_compilation_.snapshotIds()->empty());
    assert(!impl.material_compilation_.operation(exit_compile));
    const auto exit_completed = impl.engine_->execution().taskInfo(exit_task);
    assert(exit_completed && exit_completed->finished && exit_completed->state == process::ETaskState::SUCCEEDED);
    const auto exit_saved = std::ranges::find(
        impl.content_saving_->reports(),
        *exit_save,
        &ProjectSaveReport::id
    );
    assert(exit_saved != impl.content_saving_->reports().end() && exit_saved->result);
    assert(std::holds_alternative<persistence::CommitReceipt>(exit_saved->result->publication));
    assert(impl.content_saving_->pending().empty() && impl.sessions_.size() == 0);
    std::cout << "X12-09: accepted encode/compile, independent Run and GPU drain; source publication retained\n";
    std::cout << "Application: formal service assembly, frames, and asynchronous exit drain complete\n";
    // Reopen the actual Builder-created project rather than inventing a catalog entry.
    const auto default_path = root / "Initial3D/Project.luxproject";
    const auto default_project = prepareProjectOpen(default_path);
    assert(default_project && !default_project->manifest().default_scene.empty());
    // Earlier local activity fixtures still borrow this application execution owner until scope exit.
    config.project_file = default_path;
    auto initial = EditorApplication::create(config);
    assert(initial);
    auto& initial_owner = ApplicationTestAccess::implementation(**initial);
    const auto initial_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (initial_owner.sessions_.size() == 0 ||
           !std::ranges::any_of(*initial_owner.desktop_->views().describeAll(),
                               [](const auto& view) { return !view.content.sessions.empty(); }))
    {
        assert((*initial)->update());
        assert(std::chrono::steady_clock::now() < initial_deadline);
    }
    assert(initial_owner.sessions_.size() == 1);
    assert((*initial)->execute(commands::CommandId{"lux.editor.initial-scene"}));
    for (int frame{}; frame < 12; ++frame)
        assert((*initial)->update());
    assert(initial_owner.sessions_.size() == 1);
    std::cout << "Initial scene: installed project description opens one shared content; explicit menu reuses it\n";
    // A real installed factory refusal happens after content publication, not during fake model construction.
    auto original_factories = initial_owner.contributions_.snapshot();
    extensions::ContributionDraft failing_draft;
    const auto commands = original_factories.commands().entries();
    failing_draft.commands.assign(commands.begin(), commands.end());
    const auto factories = original_factories.sessions().entries();
    failing_draft.sessions.assign(factories.begin(), factories.end());
    for (const auto& entry : original_factories.views().entries())
    {
        if (entry->descriptor().type != views::ViewTypeIdView{"lux.editor.material"})
            failing_draft.views.push_back(entry);
        else
            failing_draft.views.push_back(views::ViewFactoryEntry::create(
                contracts::CodeLease::builtin(),
                entry->descriptor(),
                [](const views::ViewFactoryInput&) -> views::ViewFactoryResult<views::DetachedView> {
                    return cxx::unexpected(views::ViewFactoryFailure{
                        views::EViewFactoryError::CONSTRUCT,
                        "test.actual-material-factory",
                        17,
                        "Deliberate failure"
                    });
                }
            ));
    }
    auto failing_factories = extensions::ContributionSnapshot::prepare(std::move(failing_draft));
    assert(failing_factories && initial_owner.contributions_.enqueue(*failing_factories));
    assert(initial_owner.contributions_.applyPending());
    const auto view_count = initial_owner.desktop_->views().describeAll()->size();
    const auto partial_command = (*initial)->execute(commands::CommandId{"lux.editor.new.material"});
    assert(partial_command);
    const sessions::OpenAssetId partial_id{std::get<commands::AcceptedOperation>(*partial_command).value};
    while (!(*initial)->openStatus(partial_id)->presentation_failure)
    {
        assert((*initial)->update());
        assert(std::chrono::steady_clock::now() < initial_deadline);
    }
    const auto partial = (*initial)->openStatus(partial_id);
    assert(partial && partial->content.stage == sessions::EOpenAssetStage::PUBLISHED && !partial->view);
    const auto retained_content = initial_owner.sessions_.describe(partial->content.session);
    assert(
        retained_content && retained_content->dirty && !retained_content->binding && initial_owner.sessions_.size() == 2
    );
    assert(initial_owner.opening_.find(partial->content.session));
    assert(initial_owner.desktop_->views().describeAll()->size() == view_count);
    assert(initial_owner.contributions_.enqueue(original_factories) && initial_owner.contributions_.applyPending());
    assert((*initial)->show(partial->content.session));
    const auto recovered = initial_owner.sessions_.describe(partial->content.session);
    assert(recovered && recovered->current == retained_content->current && recovered->dirty == retained_content->dirty);
    assert(
        initial_owner.sessions_.size() == 2 && initial_owner.desktop_->views().describeAll()->size() == view_count + 1
    );
    assert((*initial)->acknowledgeOpen(partial_id));
    std::cout
        << "X12-01: real factory failure preserves published unbound content; explicit show recovers same Session\n";
}
