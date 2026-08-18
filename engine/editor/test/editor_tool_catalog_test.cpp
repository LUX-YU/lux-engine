#include <lux/engine/editor/extensions/EditorTools.hpp>
#include <lux/engine/ui/Panel.hpp>

#include <cstdio>
#include <memory>
#include <type_traits>

static_assert(!std::is_same_v<
    lux::editor::PanelId,
    lux::extensions::ContributionId>);

namespace
{
    int failures = 0;

    void expect(bool value, const char* message)
    {
        std::printf("[%s] %s\n", value ? " ok " : "FAIL", message);
        failures += value ? 0 : 1;
    }

    lux::editor::EditorPanelContributionDescriptor descriptor(
        const char* id)
    {
        lux::editor::EditorPanelContributionDescriptor value;
        value.id = lux::editor::PanelId{id};
        value.display_name = id;
        value.provider = lux::extensions::ExtensionId{"org.lux.test.editor"};
        value.create = [](const lux::editor::EditorPanelCreateContext&)
        {
            return lux::cxx::expected<
                std::unique_ptr<lux::ui::Panel>,
                lux::editor::EEditorPanelCreateError>{
                std::make_unique<lux::ui::Panel>("probe")};
        };
        return value;
    }
}

int main()
{
    int service = 42;
    lux::editor::EditorPanelCreateContext context;
    expect(context.add(service).has_value(),
           "a typed editor service is registered once");
    auto duplicate_service = context.add(service);
    expect(
        !duplicate_service && duplicate_service.error() ==
            lux::editor::EEditorServiceRegistrationError::DUPLICATE_TYPE,
        "duplicate editor service types fail loudly");

    lux::editor::EditorPanelCatalog catalog;
    auto first = catalog.add(descriptor("org.lux.test.panel"));
    expect(first.has_value(), "a valid editor panel is accepted");

    auto duplicate = catalog.add(descriptor("org.lux.test.panel"));
    expect(
        !duplicate && duplicate.error() ==
            lux::editor::EEditorPanelCatalogError::DUPLICATE_PANEL,
        "duplicate panel ids fail without replacing the factory");

    auto invalid_id = catalog.add(descriptor("Org.lux.test.invalid"));
    expect(
        !invalid_id && invalid_id.error() ==
            lux::editor::EEditorPanelCatalogError::INVALID_DESCRIPTOR,
        "panel ids enforce the editor domain's canonical spelling");

    auto missing_factory = descriptor("org.lux.test.missing-factory");
    missing_factory.create = {};
    auto missing = catalog.add(std::move(missing_factory));
    expect(
        !missing && missing.error() ==
            lux::editor::EEditorPanelCatalogError::MISSING_CREATE_CALLBACK,
        "a descriptor without a factory is rejected");

    expect(
        catalog.all().size() == 1u,
        "failed catalog transactions leave the published snapshot unchanged");
    return failures == 0 ? 0 : 1;
}
