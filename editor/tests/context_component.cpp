#include "api_contract.hpp"
#include <cassert>
#include <lux/engine/editor/EditorUiRegistrar.hpp>
#include <lux/engine/editor/FrameworkErrors.hpp>
#include <lux/engine/error/ErrorRegistry.hpp>

// Deliberately no Pane header and no UI library: every callable declared by Context links here.
int main()
{
    using namespace lux;
    assert(editor::registerFrameworkErrors());
    editor::EditorUiRegistrar factories;
    auto result = factories.registerFactory("missing", {});
    assert(!result && result.error().type == editor::Errors::EditorInvalidUiFactory);
    editor::SceneProfileRegistrar profiles;
    auto profile = profiles.registerProfile({});
    assert(!profile && profile.error().type == editor::Errors::SceneProfileInvalid);
    assert(error::ErrorRegistry::instance().find(editor::Errors::EditorInvalidWindowExtent));
    constexpr error::Error failure{editor::Errors::EditorInvalidWindowExtent};
    static_assert(failure.type == error::errorId("lux.editor.invalid_window_extent"));
}
