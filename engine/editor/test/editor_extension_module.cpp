#include <lux/engine/core/extension_abi/ModuleAbi.hpp>
#include <lux/engine/editor/extensions/EditorContributionRegistrar.hpp>
#include <lux/engine/ui/Panel.hpp>

#include <memory>

#if defined(_WIN32)
#define LUX_TEST_EDITOR_EXTENSION_EXPORT __declspec(dllexport)
#else
#define LUX_TEST_EDITOR_EXTENSION_EXPORT \
    __attribute__((visibility("default")))
#endif

namespace
{
    class FixturePanel final : public lux::ui::Panel
    {
    public:
        FixturePanel() : lux::ui::Panel("Cross-module fixture") {}

    private:
        void paint() override {}
    };
}

extern "C" LUX_TEST_EDITOR_EXTENSION_EXPORT
const lux::extensions::ExtensionModuleDescriptorV4*
luxGetExtensionModuleV4() noexcept
{
    using namespace lux::extensions;
    static constexpr ExtensionModuleDescriptorV4 descriptor{
        sizeof(ExtensionModuleDescriptorV4),
        kExtensionAbiV4,
        kEngineExtensionAbiFingerprint,
        lux::cxx::AbiStringView{"org.lux.test.editor-module"},
        ExtensionVersion{1u, 0u, 0u},
        EExtensionModuleTarget::EDITOR,
        nullptr,
        0u};
    return &descriptor;
}

extern "C" LUX_TEST_EDITOR_EXTENSION_EXPORT
lux::extensions::ExtensionRegistrationResult
luxRegisterEditorContributionsV4(
    lux::extensions::EditorContributionRegistrar& registrar) noexcept
{
    lux::editor::EditorPanelContributionDescriptor panel;
    panel.id = lux::extensions::ContributionId{
        "org.lux.test.editor-module.panel"};
    panel.display_name = "Cross-module fixture";
    panel.create = [](const lux::editor::EditorPanelCreateContext&)
        -> lux::cxx::expected<
            std::unique_ptr<lux::ui::Panel>,
            lux::editor::EEditorPanelCreateError>
    {
        return std::unique_ptr<lux::ui::Panel>{
            std::make_unique<FixturePanel>()};
    };
    if (!registrar.panels().add(std::move(panel)))
    {
        return {
            lux::extensions::EExtensionRegistrationError::INVALID_CONFIG};
    }
    return {};
}
