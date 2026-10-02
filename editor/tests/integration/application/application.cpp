#include <lux/engine/material/graph/Nodes.hpp>
#include <lux/engine/editor/application/EditorApplicationImpl.hpp>
#include <lux/engine/editor/desktop/DesktopTestAccess.hpp>
#include <lux/engine/editor/scene/OutlinerView.hpp>
#include <lux/engine/editor/scene/InspectorView.hpp>
#include <lux/engine/editor/scene/ResourceView.hpp>
#include <lux/engine/scene/SceneSystemInstaller.hpp>
#include <lux/engine/resource/asset/model/ModelAsset.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>
#include <lux/engine/material/Cooker.hpp>
#include <lux/engine/resource/asset/AssetSerDeser.hpp>
#include <lux/engine/resource/asset/storage/pak/PakArchive.hpp>
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
         {{model_id, EProjectAssetKind::MODEL, "Content/Model.recipe", "Content/Model.pak", {}, {}, "Content/Model"}},
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
    const auto material_view = std::ranges::find(
                                   impl.content_views_,
                                   material_id,
                                   &std::remove_reference_t<decltype(impl)>::ContentView::session
    )
                                   ->view;
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
    assert(app->update() && impl.save_question_ && impl.pending_saves_.empty());
    assert(impl.save_question_->target.based_on == original_save_target.based_on);
    auto choose_source = [&](ui::Pane& pane) {
        auto& question = static_cast<desktop::ReviewView&>(pane);
        assert(question.setText("Content/Beginner/Material.source"));
        assert(question.answer(desktop::EReviewChoice::SAVE));
    };
    assert(impl.desktop_->views().withView(impl.save_question_->view, choose_source));
    assert(app->update());
    const auto save_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!impl.pending_saves_.empty())
    {
        assert(std::chrono::steady_clock::now() < save_deadline);
        auto frame = app->update();
        if (!frame)
            std::cerr << "save frame: " << frame.error().domain << '\n';
        assert(frame);
        std::this_thread::yield();
    }
    assert(!impl.save_reports_.empty() && !impl.save_reports_.back().failure && impl.save_reports_.back().result);
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
    assert(impl.artifacts_.back().settled && impl.artifacts_.back().failure && !impl.artifacts_.back().ticket);
    material_action([&](lux::editor::material::MaterialView& view) { assert(view.undo() && view.requestPublication()); }
    );
    assert(app->update() && impl.artifacts_.back().ticket);
    rename_material(); // Already admitted work owns the older capture, not this live source.
    const auto newer_material = impl.sessions_.describe(material_id);
    while (!impl.artifacts_.back().settled)
    {
        assert(std::chrono::steady_clock::now() < compile_deadline);
        assert(app->update());
    }
    const auto& published_material = impl.artifacts_.back();
    if (published_material.failure)
        std::cerr << published_material.failure->domain << ": " << published_material.failure->message << '\n';
    assert(!published_material.failure && published_material.result);
    assert(std::holds_alternative<persistence::CommitReceipt>(*published_material.result));
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
    while (!impl.pending_saves_.empty())
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
            for (auto& content : impl.content_views_)
                if (content.view == material_view)
                {
                    const auto state = content.preview->status();
                    std::cerr << "Preview: prepared=" << bool(state.prepared) << " accepted=" << bool(state.accepted)
                              << " stale=" << state.stale << " diagnostic=" << state.diagnostic << '\n';
                }
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
    } while (app->phase() != EApplicationPhase::RUNNING || !impl.pending_saves_.empty());
    assert(std::filesystem::exists(impl.project_->root() / "Content/Beginner/Flow.source"));
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
    impl.result_intent_ = std::remove_reference_t<decltype(impl)>::ResultIntent{
        std::remove_reference_t<decltype(impl)>::EResultAction::ACK_MODEL,
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
    auto selected_record =
        std::ranges::find(impl.content_views_, scene_view, &std::remove_reference_t<decltype(impl)>::ContentView::view);
    auto interaction = selected_record->scene;
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
        std::ranges::find(impl.save_reports_, *saving, &std::remove_reference_t<decltype(impl)>::SavePresentation::id);
    assert(failed_save != impl.save_reports_.end() && failed_save->catalog_ticket);
    using Failures = std::remove_reference_t<decltype(impl)>::SceneFailures;
    auto retained = std::any_cast<std::shared_ptr<const Failures>>(failed_frame.error().cause);
    assert(retained && retained->values.size() == 1);
    const auto failed_instance = retained->values.front().scene;
    auto retired_failure = failing->retire();
    while (!retired_failure.complete() || std::ranges::find(impl.pending_saves_, *saving) != impl.pending_saves_.end())
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
    impl.result_intent_ = std::remove_reference_t<decltype(impl)>::ResultIntent{
        std::remove_reference_t<decltype(impl)>::EResultAction::ACK_MAINTENANCE,
        std::uint64_t{}
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
