// Defect probes assert the intended contract and return 1 on the old defect.
// Run separately from CTest; never invert these assertions or set WILL_FAIL.
#include <lux/engine/editor/detail/EditorTestAccess.hpp>
#include <lux/engine/editor/project/ProjectManifest.hpp>
#include <lux/engine/editor/metadata/CommandRegistration.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace
{
    using namespace lux::editor;

    PaneRegistration probePane()
    {
        PaneRegistration registration;
        registration.type = lux::ui::PaneTypeId{"p00.probe"};
        registration.name = "P00 probe";
        registration.create = +[](PaneManager& panes) noexcept -> PaneRegistration::CreateResult {
            if (auto* existing = panes.findFirst(lux::ui::PaneTypeIdView{"p00.probe"}))
                return std::ref(*existing);
            return panes.adopt(std::make_unique<lux::ui::Pane>(
                panes.root(), lux::ui::PaneId{"p00.instance"}, lux::ui::PaneTypeId{"p00.probe"}, "Probe"
            ));
        };
        registration.restore = +[](PaneManager& panes, const PaneState&) noexcept -> PaneRegistration::CreateResult {
            return probePane().create(panes);
        };
        return registration;
    }

    struct QueryFacts final
    {
        bool active{};
        bool released_while_active{};
        bool released{};
        bool replacement_succeeded{};
    };
    struct CodePin final
    {
        QueryFacts& facts;
        ~CodePin()
        {
            facts.released = true;
            facts.released_while_active = facts.active;
        }
    };

    bool queryReplacement(Editor& editor)
    {
        QueryFacts facts;
        CommandRegistration registration;
        registration.id = lux::ui::CommandId{"p00.query"};
        registration.label = "Query replacement";
        registration.code_lifetime = std::make_shared<CodePin>(facts);
        registration.invoke = [&facts](EditorContext& context, lux::ui::Command&) -> EditorResult<void> {
            // Keep only a stack pointer to the external witness after self-replacement.
            // No access to this callable's captures after setCommands destroys the old record.
            auto* witness = &facts;
            witness->active = true;
            witness->replacement_succeeded = bool(context.setCommands({}));
            witness->active = false;
            return {};
        };
        std::vector<CommandRegistration> entries;
        entries.push_back(std::move(registration));
        if (!editor.context().setCommands(std::move(entries)))
            return false;
        lux::ui::Command query{lux::ui::CommandIdView{"p00.query"}, lux::ui::ECommandPhase::QUERY};
        EditorTestAccess::queryCommand(editor, query);
        std::printf("replacement=%d released=%d released_during_query=%d\n",
                    facts.replacement_succeeded, facts.released, facts.released_while_active);
        return facts.replacement_succeeded && facts.released && !facts.released_while_active;
    }

    bool badDocking(Editor& editor)
    {
        auto& panes = editor.context().panes();
        if (!panes.setRegistrations({probePane()}))
            return false;
        auto pane = panes.create(lux::ui::PaneTypeIdView{"p00.probe"});
        if (!pane)
            return false;
        pane->get().setVisible(false);
        const auto before = panes.panes().size();
        const auto dock_before = editor.captureDockState();
        detail::WorkspaceData data;
        data.panes.push_back({lux::ui::PaneTypeId{"p00.probe"}, pane->get().id(), true, {}});
        constexpr char malformed[] = "[Window][";
        std::vector<std::byte> bytes(sizeof(malformed) - 1);
        std::memcpy(bytes.data(), malformed, bytes.size());
        data.dock = lux::ui::DockState{std::move(bytes)};
        const auto restored = EditorTestAccess::restoreWorkspace(editor, data);
        const auto dock_after = editor.captureDockState();
        const bool same_dock = std::ranges::equal(dock_before.bytes(), dock_after.bytes());
        std::printf("rejected=%d visible_before=0 visible_after=%d count_before=%zu count_after=%zu same_dock=%d\n",
                    !restored, pane->get().visible(), before, panes.panes().size(), same_dock);
        return !restored && !pane->get().visible() && before == panes.panes().size() && same_dock;
    }
}

int main(int argc, char** argv)
{
    if (argc != 4)
        return 2;
    const std::string mode = argv[1];
    if (mode != "C01" && mode != "C03" && mode != "C04")
        return 2;
    const auto directory = std::filesystem::path(argv[3]) /
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(directory);
    ProjectManifest manifest;
    std::array<std::uint8_t, 16> identity{};
    identity.back() = 1;
    manifest.id = lux::asset::AssetId{identity};
    manifest.name = "P00 isolated probe";
    const auto encoded = encodeProjectManifest(manifest);
    if (!encoded)
        return 2;
    EditorConfig config;
    config.project_file = directory / "Project.luxproject";
    config.plugin_root = argv[2];
    config.window.visible = false;
    config.execution = {2, 64, 64, {64}, lux::process::BlockingSchedulerConfig{2, 64}};
    {
        std::ofstream file(config.project_file, std::ios::binary);
        file << *encoded;
        if (!file)
            return 2;
    }
    if (mode == "C04")
        EditorTestAccess::failNextMenuConnection();
    auto editor = Editor::create(std::move(config));
    bool passed{};
    if (mode == "C04")
    {
        if (!editor && editor.error().domain != "object.connect")
        {
            std::fprintf(stderr, "PROBE_SETUP_ERROR: %s\n", editor.error().domain.c_str());
            return 2;
        }
        passed = !editor && editor.error().domain == "object.connect";
        std::printf("create_succeeded=%d", bool(editor));
        if (editor)
            std::printf(" outcome_succeeded=%d", bool((*editor)->outcome()));
        std::puts("");
    }
    else
    {
        if (!editor)
        {
            std::fprintf(stderr, "PROBE_SETUP_ERROR: %s\n", editor.error().domain.c_str());
            return 2;
        }
        passed = mode == "C01" ? badDocking(**editor) : queryReplacement(**editor);
    }
    std::printf("%s %s intended contract\n", passed ? "PASS" : "FAIL", mode.c_str());
    return passed ? 0 : 1;
}
