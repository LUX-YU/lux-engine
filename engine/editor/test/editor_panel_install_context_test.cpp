#include <lux/engine/editor/extensions/EditorPanels.hpp>
#include <lux/engine/ui/Panel.hpp>

#include <cstdio>
#include <memory>

namespace
{
    int failures = 0;

    void expect(bool value, const char* message)
    {
        std::printf("[%s] %s\n", value ? " ok " : "FAIL", message);
        failures += value ? 0 : 1;
    }

    [[nodiscard]] lux::editor::EditorPanelSpec spec(const char* id)
    {
        lux::editor::EditorPanelSpec result;
        result.id = lux::editor::PanelId{id};
        result.display_name = id;
        return result;
    }
}

int main()
{
    auto module = std::make_shared<lux::extensions::ModuleLifetime>(
        lux::engine::platform::DynamicLibrary{},
        lux::extensions::ExtensionId{"org.lux.test.editor"},
        lux::extensions::ExtensionVersion{1u, 0u, 0u},
        lux::extensions::ExtensionModuleOrigin{});
    const auto baseline_leases = module.use_count();

    {
        lux::editor::EditorPanelInstallContext context{module};
        auto first = context.add(
            spec("org.lux.test.panel"),
            std::make_unique<lux::ui::Panel>("probe"));
        expect(first.has_value(), "a valid panel is collected directly");
        expect(
            module.use_count() > baseline_leases,
            "the pending panel pins its provider module");

        auto duplicate = context.add(
            spec("org.lux.test.panel"),
            std::make_unique<lux::ui::Panel>("duplicate"));
        expect(
            !duplicate && duplicate.error() ==
                lux::editor::EEditorPanelInstallError::DUPLICATE_PANEL,
            "duplicate panel ids fail before UI publication");

        auto invalid = context.add(
            spec("Org.lux.test.invalid"),
            std::make_unique<lux::ui::Panel>("invalid"));
        expect(
            !invalid && invalid.error() ==
                lux::editor::EEditorPanelInstallError::INVALID_PANEL,
            "panel ids enforce canonical spelling");

        auto missing = context.add(
            spec("org.lux.test.missing"),
            std::unique_ptr<lux::ui::Panel>{});
        expect(
            !missing && missing.error() ==
                lux::editor::EEditorPanelInstallError::INVALID_PANEL,
            "null panels are rejected before publication");
    }

    expect(
        module.use_count() == baseline_leases,
        "discarding the install context releases pending leases");
    return failures == 0 ? 0 : 1;
}
