#include <lux/engine/editor/extensions/EditorContributionRegistrar.hpp>
#include <lux/engine/runtime/extensions/ExtensionModuleManager.hpp>

#include <cstdio>
#include <filesystem>
#include <memory>

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
        "the committed module enters its registration transaction");

    auto entrypoints = modules.entrypoints(
        lux::extensions::extensionId("org.lux.test.editor-module"));
    expect(
        entrypoints.editor != nullptr && entrypoints.runtime == nullptr,
        "the module exposes only the editor registrar entrypoint");

    lux::editor::EditorPanelCatalog catalog;
    auto prepare_editor =
        lux::extensions::makeEditorRegistrationAdapter(catalog);
    auto transaction = prepare_editor(entrypoints);
    expect(transaction.has_value(), "the module builds an unpublished draft");
    if (!transaction)
        return 1;
    expect(
        (*transaction)->validate().has_value(),
        "the editor contribution validates before publication");
    expect(
        (*transaction)->commit().has_value(),
        "the editor contribution publishes atomically");
    expect(
        modules.markReady(
            lux::extensions::extensionId("org.lux.test.editor-module")),
        "the module becomes READY only after catalog publication");

    auto* descriptor = catalog.find(
        lux::extensions::contributionId(
            "org.lux.test.editor-module.panel"));
    expect(descriptor != nullptr, "the cross-module panel is discoverable");
    if (!descriptor)
        return 1;
    expect(
        descriptor->provider.name() == "org.lux.test.editor-module" &&
            static_cast<bool>(descriptor->module),
        "the catalog descriptor pins its provider DLL with ModuleLease");

    lux::editor::EditorPanelCreateContext context;
    auto panel = descriptor->create(context);
    expect(
        panel.has_value() && (*panel)->title() == "Cross-module fixture",
        "factory code in the extension DLL creates a real editor panel");

    // Replaying the registrar produces a second unpublished draft. Validation
    // rejects it and the already published catalog remains unchanged.
    auto duplicate = prepare_editor(entrypoints);
    expect(duplicate.has_value(), "a duplicate draft can still be collected");
    if (duplicate)
    {
        expect(
            !(*duplicate)->validate(),
            "duplicate contribution fails during pre-publication validation");
    }
    expect(
        catalog.all().size() == 1u,
        "a failed editor transaction leaves the live catalog unchanged");

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
