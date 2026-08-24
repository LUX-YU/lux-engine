#include <lux/engine/editor/extensions/EditorPanels.hpp>
#include <lux/engine/runtime/extensions/ExtensionModuleManager.hpp>

#include <cstdio>
#include <filesystem>
#include <vector>

namespace
{
    int failures = 0;

    void expect(bool value, const char* message)
    {
        std::printf("[%s] %s\n", value ? " ok " : "FAIL", message);
        failures += value ? 0 : 1;
    }
}

int main(int argc, char** argv)
{
    if (argc != 2)
        return 2;

    const auto module_path = std::filesystem::path{argv[1]};
    lux::extensions::ExtensionModuleManager modules;
    auto prepared = lux::extensions::ExtensionModuleManager::prepare(
        lux::extensions::ExtensionModuleRequirement::fromPath(
            lux::extensions::ExtensionId{"org.lux.test.editor-module"},
            module_path,
            lux::extensions::EExtensionModuleTarget::EDITOR,
            1u,
            0u));
    expect(prepared.has_value(), "the editor-only DLL passes ABI validation");
    if (!prepared)
        return 1;

    std::vector<lux::extensions::PreparedExtensionModule> batch;
    batch.push_back(std::move(*prepared));
    auto committed = modules.commitBatch(std::move(batch));
    expect(committed.has_value(), "the editor-only DLL commits as a module");
    if (!committed)
        return 1;

    expect(
        modules.beginRegistration(
            lux::extensions::extensionId("org.lux.test.editor-module")),
        "the committed module enters registration");

    auto entrypoints = modules.entrypoints(
        lux::extensions::extensionId("org.lux.test.editor-module"));
    expect(
        entrypoints.editor_panels != nullptr &&
            entrypoints.world_systems == nullptr &&
            entrypoints.render_features == nullptr,
        "the module exposes only the direct editor-panel entrypoint");

    const auto baseline_leases = entrypoints.module.use_count();
    {
        lux::editor::EditorPanelInstallContext context{entrypoints.module};
        const auto installed = entrypoints.editor_panels(context);
        expect(
            static_cast<bool>(installed),
            "the DLL directly supplies a real Panel to the install context");
        expect(
            entrypoints.module.use_count() > baseline_leases,
            "pending panel ownership pins the provider ModuleLease");
    }
    expect(
        entrypoints.module.use_count() == baseline_leases,
        "discarding an unpublished install context releases its leases");

    expect(
        modules.markReady(
            lux::extensions::extensionId("org.lux.test.editor-module")),
        "the module becomes READY after direct panel assembly");

    auto runtime_mismatch =
        lux::extensions::ExtensionModuleManager::prepare(
            lux::extensions::ExtensionModuleRequirement::fromPath(
                lux::extensions::ExtensionId{
                    "org.lux.test.editor-module"},
                module_path,
                lux::extensions::EExtensionModuleTarget::RUNTIME,
                1u,
                0u));
    expect(
        !runtime_mismatch && runtime_mismatch.error().code ==
            lux::extensions::EExtensionModuleLoadError::TARGET_MISMATCH,
        "an editor-only DLL cannot satisfy a Runtime requirement");

    return failures == 0 ? 0 : 1;
}
