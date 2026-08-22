#include <lux/engine/extensions/ExtensionAbi.hpp>
#include <lux/engine/editor/extensions/EditorPanels.hpp>
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
const lux::extensions::ExtensionModuleDescriptorV5*
luxGetExtensionModuleV5() noexcept
{
    using namespace lux::extensions;
    static constexpr ExtensionModuleDescriptorV5 descriptor{
        sizeof(ExtensionModuleDescriptorV5),
        kExtensionAbiV5,
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
luxInstallEditorPanelsV5(
    lux::editor::EditorPanelInstallContext& context) noexcept
{
    lux::editor::EditorPanelSpec spec;
    spec.id = lux::editor::PanelId{
        "org.lux.test.editor-module.panel"};
    spec.display_name = "Cross-module fixture";
    if (!context.add(
            std::move(spec),
            std::make_unique<FixturePanel>()))
    {
        return {
            lux::extensions::EExtensionRegistrationError::INVALID_CONFIG};
    }
    return {};
}
