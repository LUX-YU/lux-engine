#include "ToolTestAccess.hpp"
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/project/PluginManager.hpp>
#include "../EditorTestFactory.hpp"
#include <lux/engine/editor/detail/EditorTestAccess.hpp>
#include "../TestExit.hpp"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <lux/engine/editor/Editor.hpp>
#include <lux/engine/editor/material/MaterialEditor.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/object/detail/MessageEnvelope.hpp>
#include <lux/engine/process/TaskScope.hpp>
#include <thread>

using namespace lux::editor;
namespace material = lux::editor::material;

namespace
{
    lux::asset::AssetId identity(unsigned char value)
    {
        std::array<std::uint8_t, 16> bytes{};
        bytes.back() = value;
        return lux::asset::AssetId{bytes};
    }

    void write(const std::filesystem::path& path, std::string_view bytes)
    {
        std::ofstream output(path, std::ios::binary);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        assert(output.good());
    }

    void createProject(const std::filesystem::path& root)
    {
        assert(!std::filesystem::exists(root));
        std::filesystem::create_directories(root);
        // An actual source graph with no output produces a normal compiler failure.
        // Completion lifetime must be correct for errors as well as successful SPIR-V.
        lux::material::MaterialSource source{identity(2), "Review material", {}};
        const auto encoded = lux::material::encodeMaterialSource(source);
        assert(encoded);
        write(root / "Material.luxmaterial", *encoded);

        source.id = identity(3);
        const auto other = lux::material::encodeMaterialSource(source);
        assert(other);
        write(root / "Other.luxmaterial", *other);

        ProjectManifest project{identity(1),
                                "D2-C review",
                                {},
                                {{identity(2),
                                  EProjectAssetKind::MATERIAL_GRAPH,
                                  "Material.luxmaterial",
                                  {},
                                  projectContentDigest(std::as_bytes(std::span{encoded->data(), encoded->size()})),
                                  {},
                                  "Material"}}};
        project.assets.push_back({identity(3),
                                  EProjectAssetKind::MATERIAL_GRAPH,
                                  "Other.luxmaterial",
                                  {},
                                  projectContentDigest(std::as_bytes(std::span{other->data(), other->size()})),
                                  {},
                                  "Other"});
        project.plugins.push_back({"lux.builtin.scene_render", 1, {}});
        const auto manifest = encodeProjectManifest(project);
        assert(manifest);
        write(root / "Project.luxproject", *manifest);
    }

}

int main(int argc, char** argv)
{
    assert(argc == 3);
    const std::string mode = argv[1];
    const auto root = std::filesystem::path(argv[2]) / std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    createProject(root);
    EditorConfig config;
    config.project_file = root / "Project.luxproject";
    config.plugin_root = LUX_TEST_PLUGIN_ROOT;
    config.window.visible = false;
    config.execution = {2, 64, 64, {64}, lux::process::BlockingSchedulerConfig{2, 64}};
    auto made = EditorTestFactory::create(std::move(config));
    assert(made);
    auto& editor = **made;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(40);
    auto pump = [&] {
        assert(std::chrono::steady_clock::now() < deadline);
        EditorTestAccess::turn(editor);
        assert(editor.outcome());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    };
    auto until = [&](auto&& ready) { do { pump(); } while (!ready()); };
    auto tool = EditorTestFactory::show<material::MaterialEditor>(editor);
    assert(tool);
    auto& material = tool->get();
    assert(material.openAsset(identity(2)));
    const auto loading = editor.context().openAsset(identity(2));
    assert(loading && &loading->get() == &material);
    auto other = editor.context().openAsset(identity(3));
    assert(other && &other->get() != &material);
    const auto other_id = other->get().id();
    const auto again = editor.context().openAsset(identity(3));
    assert(again && &again->get() == &other->get());
    const auto settings = editor.context().panes().create(lux::ui::PaneTypeIdView{"lux.editor.settings"});
    const auto same_settings = editor.context().panes().create(lux::ui::PaneTypeIdView{"lux.editor.settings"});
    assert(settings && same_settings && &settings->get() == &same_settings->get());
    const auto extra = editor.context().panes().create(lux::ui::PaneTypeIdView{material::kMaterialEditorType});
    assert(extra && &extra->get() != &material && extra->get().id() != material.id());
    const auto extra_id = extra->get().id();
    assert(editor.context().panes().erase(extra_id.view()));
    until([&] { return material.assetStatus().phase == EAssetEditPhase::IDLE; });
    assert(!material.assetStatus().failure);
    assert(editor.context().panes().find(other_id.view()));
    const auto pane = material.id();
    if (mode == "compile-notice")
    {
        struct Receiver : lux::object::LuxObject
        {
            Receiver(lux::object::ObjectDispatcherRef dispatcher, unsigned& value) : LuxObject(dispatcher), count(value) {}
            void completed(const lux::process::TaskId&) noexcept { ++count; }
            unsigned& count;
        };
        auto queue = lux::object::ObjectMessageQueue::create(16);
        assert(queue);
        unsigned notices{}, second{}, queued{}, retired{};
        lux::process::TaskId notified;
        auto receiver = std::make_unique<Receiver>(queue->dispatcherRef(), retired);
        Receiver live(queue->dispatcherRef(), queued);
        auto a = lux::object::LuxObject::connect(&material, &material::MaterialEditor::compileFinished,
            [&](lux::process::TaskId id) noexcept {
                ++notices;
                notified = id;
                auto ack = material.requestCompile();
                assert(!ack && ack.error().code == EEditorError::BUSY);
            });
        auto b = lux::object::LuxObject::connect(&material, &material::MaterialEditor::compileFinished,
            [&](lux::process::TaskId id) noexcept { assert(id == notified); ++second; });
        auto c = lux::object::LuxObject::connect(&material, &material::MaterialEditor::compileFinished,
            &live, &Receiver::completed, lux::object::EDelivery::QUEUED);
        auto d = lux::object::LuxObject::connect(&material, &material::MaterialEditor::compileFinished,
            receiver.get(), &Receiver::completed, lux::object::EDelivery::QUEUED);
        assert(a && b && c && d);
        auto compile = material.requestCompile();
        assert(compile);
        until([&] { return notices == 1; });
        assert(second == 1 && notified == *compile && material.compileStatus(*compile));
        receiver.reset();
        receiver = std::make_unique<Receiver>(queue->dispatcherRef(), retired);
        static_cast<void>(queue->dispatchPending());
        assert(queued == 1 && retired == 0);
    }
    else if (mode == "close-intent")
    {
        const auto pane_id = material.id();
        material.lux::ui::Pane::requestClose();
        assert(material.visible());
        until([&] { return editor.context().panes().find(pane_id.view()) == nullptr; });
        auto reopened = EditorTestFactory::show<material::MaterialEditor>(editor);
        assert(reopened && reopened->get().id() != pane_id);

    }
    else
    {
        assert(mode == "exit-review");
        assert(toolTest(material).rename("Unsaved material"));
        auto flow = EditorTestFactory::show<flowforge::FlowForgeEditor>(editor);
        assert(flow && flow->get().newAsset());
        until([&] { return flow->get().assetStatus().phase == EAssetEditPhase::IDLE; });
        const auto material_state = material.historyView()->history.current;
        editor.requestExit();
        until([&] { return material.assetStatus().phase == EAssetEditPhase::REVIEW &&
                           flow->get().assetStatus().phase == EAssetEditPhase::REVIEW; });
        assert(material.reviewAsset(EAssetChangeDecision::DISCARD));
        assert(flow->get().reviewAsset(EAssetChangeDecision::CANCEL));
        until([&] { return material.assetStatus().phase == EAssetEditPhase::IDLE; });
        assert(!editor.closing() && material.assetStatus().phase == EAssetEditPhase::IDLE);
        assert(material.historyView()->history.current == material_state && editor.findPane(pane.view()));
        assert(material.assetStatus().phase == EAssetEditPhase::IDLE);
    }
    TestExit exit;
    exit.request(editor);
    until([&] { exit.poll(); return editor.closing(); });
    assert(editor.exec() == 0);
    std::printf("PASS %s fixed-tool review, completion ownership and shutdown\n", mode.c_str());
}
