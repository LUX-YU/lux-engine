#include <cassert>
#include <lux/engine/error/ErrorRegistry.hpp>
#include <string>
#include <string_view>

#if defined(LUX_TEST_PROJECT_ERRORS)
#include <lux/engine/editor/ProjectErrors.hpp>
constexpr auto descriptor = lux::editor::Errors::ProjectManifestDescriptor;
constexpr auto registerCatalog = lux::editor::registerProjectErrors;
#elif defined(LUX_TEST_CONTEXT_ERRORS)
#include <lux/engine/editor/ContextErrors.hpp>
constexpr auto descriptor = lux::editor::Errors::EditorRecursiveServiceFactoryDescriptor;
constexpr auto registerCatalog = lux::editor::registerContextErrors;
#elif defined(LUX_TEST_UI_ERRORS)
#include <lux/engine/editor/EditorUiErrors.hpp>
constexpr auto descriptor = lux::editor::Errors::EditorGlfwInitializationFailedDescriptor;
constexpr auto registerCatalog = lux::editor::registerEditorUiErrors;
#elif defined(LUX_TEST_APP_ERRORS)
#include <lux/engine/editor/AppErrors.hpp>
constexpr auto descriptor = lux::editor::Errors::EditorInvalidWindowExtentDescriptor;
constexpr auto registerCatalog = lux::editor::registerAppErrors;
#else
#error Select exactly one component catalog
#endif

int main(int argc, char** argv)
{
    using namespace lux;
    auto& registry = error::ErrorRegistry::instance();
    constexpr auto id = error::errorId(descriptor.name);
    assert(!registry.find(id));
    if (argc == 2)
    {
        assert(std::string_view(argv[1]) == "--collision");
        auto conflict = descriptor;
        const std::string conflict_message = std::string{"Conflicting declaration: "} + std::string{descriptor.message};
        conflict.message = conflict_message;
        assert(registry.registerType(conflict));
        const auto result = registerCatalog();
        assert(!result);
        assert(result.error().args[0] == id);
        assert(result.error().args[1] == static_cast<std::uint64_t>(error::ERegistrationError::DEFINITION_MISMATCH));
        assert(registry.find(id)->message == conflict.message);
        assert(!registerCatalog());
        return 0;
    }

    assert(argc == 1 && registerCatalog());
    const auto* definition = registry.find(id);
    assert(definition);
    assert(definition->name == descriptor.name && definition->message == descriptor.message);
    assert(definition->arguments == descriptor.arguments && definition->recovery == descriptor.recovery);
    assert(registerCatalog() && registry.find(id) == definition);
    assert(error::format({id, {17}}).find("Unknown error") == std::string::npos);

#if defined(LUX_TEST_APP_ERRORS)
    // Process owns and explicitly registers its catalog independently of Editor.
    assert(!registry.find(error::errorId("lux.process.execution.0")));
    assert(!registry.find(error::errorId("lux.process.execution.unknown")));
#endif

    // Linking a component does not register unrelated component catalogs.
    constexpr std::string_view domain_examples[]{
        "lux.editor.project.manifest",
        "lux.editor.recursive_service_factory",
        "lux.editor.glfw_initialization_failed",
        "lux.editor.invalid_window_extent"
    };
    for (const auto name : domain_examples)
    {
        if (name != descriptor.name)
        {
            assert(!registry.find(error::errorId(name)));
        }
    }
}
