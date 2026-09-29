#include "ToolTestAccess.hpp"
#include <lux/engine/editor/detail/EditorTestAccess.hpp>
#include <lux/engine/editor/assets/AssetImporter.hpp>
#include "EditorTestFactory.hpp"
#include "TestExit.hpp"
#include "../common/ControlsTestAccess.hpp"
#include "flow_metadata.hpp"
#include "entity_checks.hpp"
#include "scene_save_checks.hpp"
#include "run_checks.hpp"
#include "workspace_checks.hpp"
#include <lux/engine/editor/AssetOpenRequest.hpp>
#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/material/graph/Nodes.hpp>
#include <fstream>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/scene/WorldLoadingSystem.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/editor/metadata/ConfigurationValue.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/function/render/features/postprocess/TonemapOperation.hpp>
#include <thread>

using namespace lux::editor;
int main(int argc, char** argv)
{
    assert(argc >= 3);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string mode = argv[2];
    EditorConfig config;
    config.project_file = argv[1];
    config.plugin_root = LUX_TEST_PLUGIN_ROOT;
    config.window.visible = false;
    config.execution = {2, 64, 64, {64}, lux::process::BlockingSchedulerConfig{2, 64}};
    auto created = EditorTestFactory::create(std::move(config));
    if (!created)
        std::fprintf(stderr, "%s: %s\n", created.error().domain.c_str(), created.error().message.c_str());
    assert(created);
    auto& editor = **created;
    auto& context = editor.context();
    auto& renderer = context.renderRuntime();
    auto& resources = context.renderResources();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(80);
    auto pump = [&] {
        assert(std::chrono::steady_clock::now() < deadline);
        EditorTestAccess::turn(editor);
        if (!editor.outcome())
            std::fprintf(stderr, "editor: %s\n", editor.outcome().error().domain.c_str());
        assert(editor.outcome());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    };
    auto until = [&](auto&& ready) {
        do
        {
            pump();
        } while (!ready());
    };
    auto shown = EditorTestFactory::show<scene::SceneEditor>(editor);
    assert(shown);
    auto& scene = shown->get();
    const auto pane_id = scene.id();
    assert(!scene.historyId().value && !toolTest(scene).instance().valid());
    const auto asset = context.project().manifest().assets.front().id;
    assert(scene.openAsset(asset));
    until([&] { return scene.assetStatus().phase == EAssetEditPhase::IDLE; });
    if (scene.assetStatus().failure)
        std::fprintf(stderr, "scene: %s\n", scene.assetStatus().failure->domain.c_str());
    assert(!scene.assetStatus().failure && toolTest(scene).instance().valid());
    until([&] {
        if (!bool(context.engine().sceneRuntime().borrowInstance(toolTest(scene).instance())))
            return false;
        if (!lux::scene::RenderSceneState::find(
                std::as_const(context.engine().sceneRuntime()).borrowInstance(toolTest(scene).instance())->get(),
                toolTest(scene).selectedRenderSystem()
            ))
            return true;
        const auto snapshot = toolTest(scene).resources();
        if (!snapshot || snapshot->rows.empty())
            return false;
        for (const auto& row : snapshot->rows)
        {
            if (row.state == lux::scene::ERenderAssetState::FAILED)
                std::fprintf(stderr, "resource failed\n");
            assert(row.state != lux::scene::ERenderAssetState::FAILED);
            if (row.state != lux::scene::ERenderAssetState::READY)
                return false;
        }
        return true;
    });
    const auto original_instance = toolTest(scene).instance();
    const auto original_history = scene.historyId();
    assert(scene.openAsset(asset));
    assert(
        &EditorTestFactory::show<scene::SceneEditor>(editor)->get() == &scene &&
        toolTest(scene).instance() == original_instance && scene.historyId() == original_history
    );

    if (mode == "new-assets")
    {
        {
            lux::ui::Pane
                pane(editor, lux::ui::PaneId{"test/configurations"}, lux::ui::PaneTypeId{"test"}, "Configurations");
            lux::ui::Layout fields(pane, lux::ui::ElementId{"fields"});
            std::size_t checked{};
            for (const auto& registration : context.configurationEditors())
            {
                auto value = ConfigurationValue::create(registration, registration.code_lifetime);
                assert(value);
                auto controls = value->createElement(fields, lux::ui::ElementId{registration.schema_name});
                if (!controls)
                    std::fprintf(
                        stderr,
                        "configuration: %s %s\n",
                        registration.schema_name,
                        controls.error().message.c_str()
                    );
                assert(controls);
                std::vector<std::byte> before, defaults;
                assert(value->encode(before) && registration.codec.encode_default(defaults) && before == defaults);
                if (registration.codec.type == lux::cxx::typeToken<lux::render::TonemapCommConfig>())
                {
                    auto* layout = (*controls)->firstChild();
                    lux::ui::NumericEdit* exposure{};
                    for (auto* child = layout->firstChild(); child; child = child->nextSibling())
                        if (auto* number = dynamic_cast<lux::ui::NumericEdit*>(child);
                            number && number->id().name() == "exposure")
                            exposure = number;
                    assert(exposure);
                    exposure->setValue(2.5F);
                    assert(lux::ui::ControlsTestAccess::edited(*exposure, {.changed = true}).complete());
                    assert(static_cast<lux::render::TonemapCommConfig*>(value->data())->exposure == 2.5F);
                    std::vector<std::byte> modified;
                    assert(value->encode(modified) && modified != before);
                }
                ++checked;
            }
            assert(checked >= 29);
        }
        auto empty_material = EditorTestFactory::show<material::MaterialEditor>(editor);
        auto empty_flow = EditorTestFactory::show<flowforge::FlowForgeEditor>(editor);
        assert(empty_material && empty_flow);
        assert(!toolTest(empty_material->get()).rename("No active material"));
        assert(!toolTest(empty_flow->get()).rename("No active flow"));
        const auto check_graph = [&](auto& tool, std::string_view first_path, std::string_view copy_path) {
            assert(tool.newAsset());
            until([&] { return tool.assetStatus().phase == EAssetEditPhase::IDLE; });
            assert(!tool.assetStatus().failure && toolTest(tool).rename("Created in memory"));
            const auto first = tool.assetId();
            auto saved = tool.requestSaveAs(first_path);
            assert(saved);
            until([&] { return std::holds_alternative<SaveSucceeded>(*tool.saveStatus(*saved)); });
            assert(tool.acknowledgeSave(*saved));
            const auto baseline = tool.persistedState();
            assert(baseline && !tool.hasUnsavedChanges());
            assert(toolTest(tool).rename("Captured failure and retry"));
            const auto dirty = tool.historyView()->history.current;
            const auto* entry = context.project().asset(first);
            assert(entry);
            const auto source_path = std::filesystem::path(argv[1]).parent_path() / entry->source_path;
            const auto lock = CreateFileW(
                source_path.c_str(),
                GENERIC_READ,
                FILE_SHARE_READ,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr
            );
            assert(lock != INVALID_HANDLE_VALUE);
            auto failing = tool.requestSave("P01 real publication failure");
            assert(failing);
            until([&] { return std::holds_alternative<SaveRetryable>(*tool.saveStatus(*failing)); });
            assert(tool.persistedState() == baseline && tool.hasUnsavedChanges());
            assert(tool.historyView()->history.current == dirty);
            assert(CloseHandle(lock));
            assert(tool.retrySave(*failing));
            until([&] { return std::holds_alternative<SaveSucceeded>(*tool.saveStatus(*failing)); });
            assert(tool.persistedState()->state == dirty && !tool.hasUnsavedChanges());
            assert(tool.acknowledgeSave(*failing));
            const auto before_reload = tool.historyView()->history.current;
            const auto before_checkpoint = tool.persistedState();
            std::ifstream original(source_path, std::ios::binary);
            const std::string original_bytes{std::istreambuf_iterator<char>{original}, {}};
            original.close();
            {
                std::ofstream corrupt(source_path, std::ios::binary | std::ios::trunc);
                corrupt << "invalid candidate";
            }
            assert(tool.reloadAsset());
            until([&] { return tool.assetStatus().phase == EAssetEditPhase::IDLE; });
            assert(tool.assetStatus().failure && tool.historyView()->history.current == before_reload);
            assert(tool.persistedState() == before_checkpoint);
            {
                std::ofstream restore(source_path, std::ios::binary | std::ios::trunc);
                restore << original_bytes;
            }
            const auto history = tool.historyId();
            auto copied = tool.requestSaveAs(copy_path);
            assert(copied);
            assert(!toolTest(tool).rename("Must not edit the captured clone"));
            until([&] { return tool.historyId() != history; });
            assert(tool.assetId() != first && std::holds_alternative<SaveSucceeded>(*tool.saveStatus(*copied)));
            assert(tool.acknowledgeSave(*copied));
            assert(tool.openAsset(first));
            until([&] { return tool.assetStatus().phase == EAssetEditPhase::IDLE; });
            assert(!tool.assetStatus().failure && tool.assetId() == first);
        };
        check_graph(empty_material->get(), "/Project/New.luxmaterial", "/Project/Copy.luxmaterial");
        check_graph(empty_flow->get(), "/Project/New.luxflow", "/Project/Copy.luxflow");
        assert(scene.newAsset());
        if (scene.assetStatus().phase == EAssetEditPhase::REVIEW)
            assert(scene.reviewAsset(EAssetChangeDecision::DISCARD));
        assert(scene.assetStatus().phase == EAssetEditPhase::CONFIGURING);
        lux::simulation::SimulationDescriptionBuilder simulation;
        auto sim = std::move(simulation).build();
        assert(sim);
        auto simulation_description = std::make_shared<const lux::simulation::SimulationDescription>(std::move(*sim));
        lux::scene::SceneDescriptionBuilder incomplete;
        auto invalid = std::move(incomplete).buildResolved();
        assert(invalid);
        const auto original_checkpoint = scene.persistedState();
        assert(scene.createAsset("Missing loading", {}, simulation_description, *invalid));
        assert(editor.update({}, nullptr)); // Queue adoption without drawing or applying the structure batch.
        assert(scene.assetStatus().phase == EAssetEditPhase::PREPARING && !scene.assetStatus().failure);
        assert(toolTest(scene).instance() == original_instance && scene.historyId() == original_history);
        editor.applyPendingChanges();
        assert(scene.assetStatus().phase == EAssetEditPhase::CONFIGURING && scene.assetStatus().failure);
        assert(toolTest(scene).instance() == original_instance && scene.historyId() == original_history);
        assert(scene.persistedState() == original_checkpoint);
        lux::scene::SceneDescriptionBuilder description;
        const auto loader = lux::scene::worldLoadingSystemRegistration();
        std::vector<std::byte> configuration;
        const lux::scene::WorldLoadingConfiguration boot{{{0}}};
        assert(loader.configuration.encode(&boot, configuration));
        assert(description.addSystem(
            {1},
            "loading",
            loader.type,
            1,
            loader.description->configuration_schema_name,
            1,
            configuration
        ));
        assert(description.bindRequirement({1}, "world_loading", "world-storage"));
        auto desc = std::move(description).buildResolved();
        assert(desc);
        assert(scene.createAsset("Empty CPU scene", {}, simulation_description, *desc));
        until([&] { return scene.assetStatus().phase == EAssetEditPhase::IDLE; });
        assert(
            !scene.assetStatus().failure && toolTest(scene).instance().valid() &&
            !lux::scene::RenderSceneState::find(
                std::as_const(context.engine().sceneRuntime()).borrowInstance(toolTest(scene).instance())->get(),
                toolTest(scene).selectedRenderSystem()
            )
        );
        assert(scene.hasUnsavedChanges());
        const auto first = scene.assetId();
        const auto first_history = scene.historyId();
        auto save = scene.requestSaveAs("/Project/New.luxscene");
        assert(save);
        until([&] { return std::holds_alternative<SaveSucceeded>(*scene.saveStatus(*save)); });
        assert(scene.acknowledgeSave(*save));
        assert(scene.assetId() == first && scene.historyId() == first_history && context.project().asset(first));
        auto copy = scene.requestSaveAs("/Project/Copy.luxscene");
        assert(copy);
        until([&] { return scene.assetStatus().phase == EAssetEditPhase::IDLE; });
        assert(!scene.assetStatus().failure && scene.assetId() != first && scene.historyId() != first_history);
        assert(std::holds_alternative<SaveSucceeded>(*scene.saveStatus(*copy)));
        assert(scene.acknowledgeSave(*copy));
        const auto second = scene.assetId();
        assert(context.project().asset(first) && context.project().asset(second));
        assert(scene.openAsset(first));
        until([&] { return scene.assetStatus().phase == EAssetEditPhase::IDLE; });
        assert(!scene.assetStatus().failure && scene.assetId() == first && scene.id() == pane_id);
        workspace_checks::run(editor, until);
    }
    else if (mode == "save-preservation" || mode == "save-retry" || mode == "save-partial")
    {
        SceneSaveChecks checks;
        until([&] { return bool(context.engine().sceneRuntime().borrowInstance(toolTest(scene).instance())); });
        checks.begin(scene, mode);
        until([&] { return checks.finished(scene); });
        assert(scene.reloadAsset());
        assert(scene.assetStatus().phase == EAssetEditPhase::REVIEW);
        assert(scene.reviewAsset(EAssetChangeDecision::DISCARD));
        until([&] { return scene.assetStatus().phase == EAssetEditPhase::IDLE; });
        assert(
            !scene.assetStatus().failure && toolTest(scene).instance() != original_instance && scene.id() == pane_id
        );
        until([&] { return bool(context.engine().sceneRuntime().borrowInstance(toolTest(scene).instance())); });
        checks.reopened(scene);
    }
    else if (mode == "fixed-run")
    {
        ScenePlaybackChecks checks;
        checks.begin(scene, renderer, editor.context().renderResources());
        until([&] { return checks.poll(scene, renderer); });
        const auto run = scene.runStatus().id;
        until([&] { return scene.runStatus().state == scene::EPlaybackState::RUNNING; });
        assert(scene.pauseRun(run));
        assert(editor.update({}, nullptr));
        assert(scene.stopRun(run));
        editor.applyPendingChanges();
        until([&] {
            const auto status = scene.runStatus();
            assert(status.result && status.state != scene::EPlaybackState::PAUSED);
            return status.state == scene::EPlaybackState::FINISHED;
        });
        // Distinct assets own distinct tools and running scenes. Pausing/closing one must not stop the other.
        until([&] { return bool(context.engine().sceneRuntime().borrowInstance(toolTest(scene).instance())); });
        const auto original_asset = scene.assetId();
        const auto copy = scene.requestSaveAs("/Project/Independent.luxscene");
        assert(copy);
        until([&] { return scene.assetStatus().phase == EAssetEditPhase::IDLE; });
        assert(!scene.assetStatus().failure && scene.assetId() != original_asset);
        assert(std::holds_alternative<SaveSucceeded>(*scene.saveStatus(*copy)));
        assert(scene.acknowledgeSave(*copy));
        const auto other_pane = context.openAsset(original_asset);
        assert(other_pane);
        auto& other = static_cast<scene::SceneEditor&>(other_pane->get());
        assert(&other != &scene && other.id() != scene.id());
        until([&] { return other.assetStatus().phase == EAssetEditPhase::IDLE; });
        assert(!other.assetStatus().failure);
        const auto first_run = scene.play();
        const auto other_run = other.play();
        assert(first_run && other_run);
        until([&] { return scene.runStatus().steps >= 2 && other.runStatus().steps >= 2; });
        assert(toolTest(scene).instance() != toolTest(other).instance());
        assert(other.pauseRun(other.runStatus().id));
        until([&] { return other.runStatus().state == scene::EPlaybackState::PAUSED; });
        const auto paused_step = other.runStatus().steps;
        const auto active_step = scene.runStatus().steps;
        until([&] { return scene.runStatus().steps >= active_step + 3; });
        assert(other.runStatus().steps == paused_step);
        const auto closed_id = other.id();
        other.requestClose();
        until([&] { return context.panes().find(closed_id.view()) == nullptr; });
        const auto after_close = scene.runStatus().steps;
        until([&] { return scene.runStatus().steps >= after_close + 2; });
        assert(scene.stopRun(scene.runStatus().id));
        until([&] { return scene.runStatus().state == scene::EPlaybackState::FINISHED; });
        std::puts("PASS two scene tools: separate assets/instances, pause and close leave the other Run advancing");
    }
    else if (mode == "material-gui" || mode == "flow-gui")
    {
        const auto kind = mode == "flow-gui" ? EProjectAssetKind::FLOW_GRAPH : EProjectAssetKind::MATERIAL_GRAPH;
        const auto& entries = context.project().manifest().assets;
        const auto selected = std::ranges::find(entries, kind, &ProjectAssetEntry::kind);
        assert(selected != entries.end());
        if (mode == "flow-gui")
        {
            auto tool = EditorTestFactory::show<flowforge::FlowForgeEditor>(editor);
            assert(tool);
            const_cast<lux::flowforge::FlowSourceEnvironment&>(toolTest(tool->get()).metadata()) =
                flowMetadata(std::make_shared<MetadataOwner>());
            assert(tool->get().openAsset(selected->id));
        }
        AssetOpenRequest activate{selected->id};
        assert(lux::object::routeEvent(scene, editor, activate));
        pump();
        if (mode == "flow-gui")
        {
            auto tool = EditorTestFactory::show<flowforge::FlowForgeEditor>(editor);
            assert(tool);
            auto& flow = tool->get();
            until([&] { return flow.assetStatus().phase == EAssetEditPhase::IDLE; });
            assert(!flow.assetStatus().failure && flow.historyId().value);
            assert(flow.assetId() == selected->id && flow.content());
            const auto history = scene.historyId();
            assert(toolTest(flow).rename("Independent flow history"));
            assert(flow.undo() && scene.historyId() == history);
            unsigned compile_notices{};
            auto connection = lux::object::LuxObject::connect(
                &flow,
                &flowforge::FlowForgeEditor::compileFinished,
                [&](lux::process::TaskId) noexcept { ++compile_notices; }
            );
            assert(connection);
#if defined(D2_FLOW_LINKER)
            const auto compile = flow.requestCompile(D2_FLOW_LINKER);
            assert(compile);
            assert(context.execution().waitUntil([&]() noexcept {
                const auto info = context.execution().taskInfo(*compile);
                return info && info->finished.has_value();
            }));
            assert(context.execution().collectCompletions()); // Terminal fact precedes owner delivery.
            assert(context.execution().dispatchTaskEvents());
            assert(context.execution().collectCompletions());
            assert(compile_notices == 0);
            assert(editor.update({}, nullptr));
            assert(compile_notices == 0);
            editor.applyPendingChanges();
            assert(compile_notices == 1);
#else
            std::fputs("flow-gui requires the toolchain test group\n", stderr);
            return 2;
#endif
        }
        else
        {
            auto tool = EditorTestFactory::show<material::MaterialEditor>(editor);
            assert(tool);
            auto& material = tool->get();
            until([&] { return material.assetStatus().phase == EAssetEditPhase::IDLE && material.historyId().value; });
            assert(material.assetId() == selected->id && material.content());
            const auto material_pane = material.id();
            const auto before = material.historyId();
            auto compile = material.requestCompile();
            assert(compile);
            assert(context.execution().waitUntil([&]() noexcept {
                const auto info = context.execution().taskInfo(*compile);
                return info && info->finished.has_value();
            }));
            assert(context.execution().collectCompletions()); // Terminal fact precedes owner delivery.
            assert(context.execution().dispatchTaskEvents());
            assert(context.execution().collectCompletions());
            assert(std::holds_alternative<material::MaterialCompilePending>(*material.compileStatus(*compile)));
            assert(!toolTest(material).previewInstance().valid());
            assert(editor.update({}, nullptr));
            assert(std::holds_alternative<material::MaterialCompilePending>(*material.compileStatus(*compile)));
            assert(!toolTest(material).previewInstance().valid());
            editor.applyPendingChanges();
            until([&] {
                return !std::holds_alternative<material::MaterialCompilePending>(*material.compileStatus(*compile));
            });
            assert(std::holds_alternative<material::MaterialCompileSucceeded>(*material.compileStatus(*compile)));
            until([&] { return toolTest(material).previewStatus() == "Static preview"; });
            if (mode == "material-gui")
            {
                const auto other =
                    std::ranges::find(entries, "Unfinished.luxmaterial", &ProjectAssetEntry::source_path);
                assert(other != entries.end());
                assert(toolTest(material).rename("Do not lose this graph"));
                const auto current = material.historyView()->history.current;
                AssetOpenRequest switch_to{other->id};
                assert(lux::object::routeEvent(scene, editor, switch_to));
                material::MaterialEditor* second{};
                until([&] {
                    for (const auto& pane : context.panes().panes())
                    {
                        if (pane->type().view() != lux::ui::PaneTypeIdView{material::kMaterialEditorType})
                            continue;
                        auto& candidate = static_cast<material::MaterialEditor&>(*pane);
                        if (candidate.assetId() == other->id && candidate.assetStatus().phase == EAssetEditPhase::IDLE)
                            second = &candidate;
                    }
                    return second != nullptr;
                });
                assert(second != &material && second->id() != material_pane);
                assert(material.historyView()->history.current == current && material.assetId() == selected->id);
                assert(material.historyId() == before && toolTest(material).previewStatus() != "Not compiled");
                const auto count = context.panes().panes().size();
                const auto reopened = context.openAsset(other->id);
                assert(reopened && &reopened->get() == second && context.panes().panes().size() == count);
                const auto original = context.openAsset(selected->id);
                assert(original && &original->get() == &material);
                // Explicitly replacing one window's content still uses its own save review.
                assert(material.openAsset(other->id));
                assert(material.assetStatus().phase == EAssetEditPhase::REVIEW);
                assert(material.reviewAsset(EAssetChangeDecision::CANCEL));
                assert(material.historyView()->history.current == current && material.assetId() == selected->id);
            }
        }
    }
    else if (mode == "factory-rollback")
    {
        using Transform = lux::simulation::ecs::Transform3D;
        const auto object = sceneObjects(scene).front().object;
        assert(toolTest(scene).setField<Transform>(
            *toolTest(scene).writeTarget(object),
            "translation",
            "Translation",
            [](auto& value) { return &value.translation; },
            Eigen::Vector3d{3, 4, 5}
        ));
        const auto retained = scene.historyView()->history.current;
        const auto path = context.project().root() / context.project().asset(asset)->source_path;
        std::ifstream input(path, std::ios::binary);
        const std::string bytes{std::istreambuf_iterator<char>{input}, {}};
        input.close();
        {
            std::ofstream output(path, std::ios::binary);
            output << "broken scene source";
        }
        assert(scene.reloadAsset());
        assert(scene.reviewAsset(EAssetChangeDecision::DISCARD));
        until([&] { return scene.assetStatus().phase == EAssetEditPhase::IDLE; });
        assert(
            scene.assetStatus().failure && toolTest(scene).instance() == original_instance &&
            scene.historyView()->history.current == retained
        );
        {
            std::ofstream output(path, std::ios::binary);
            output.write(bytes.data(), bytes.size());
        }
        auto material = EditorTestFactory::show<material::MaterialEditor>(editor);
        auto flow = EditorTestFactory::show<flowforge::FlowForgeEditor>(editor);
        assert(material && flow);
        assert(material->get().newAsset());
        assert(flow->get().newAsset());
        until([&] { return material->get().historyId().value && flow->get().historyId().value; });
        assert(material->get().requestCompile());
        const auto saved_path = context.project().root() / "Content/Destructor.luxscene";
        assert(scene.requestSaveAs("/Project/Destructor.luxscene"));
        assert(editor.update({}, nullptr)); // Leave structural callbacks queued while destroying their targets.
        created->reset(); // Accepted save, pending compile, three real tools and a live scene view; no manual close.
        assert(std::filesystem::exists(saved_path));
        std::printf("PASS case=%s direct destruction preserves save and drains compile/view work\n", mode.c_str());
        return 0;
    }
    else
        assert(false && "unhandled test mode");

    TestExit exit;
    exit.request(editor);
    until([&] {
        exit.poll();
        return editor.closing();
    });
    assert(editor.exec() == 0);
    std::printf("PASS case=%s fixed tool, real owner maintenance and resource shutdown\n", mode.c_str());
}
