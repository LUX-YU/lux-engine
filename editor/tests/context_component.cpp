#include "api_contract.hpp"
#include <cassert>
#include <lux/engine/editor/ContextErrors.hpp>
#include <lux/engine/editor/EditorComposition.hpp>
#include <lux/engine/error/ErrorRegistry.hpp>

// Deliberately no Pane header and no UI library: every callable declared by Context links here.
int main()
{
    using namespace lux;
    assert(editor::registerContextErrors());
    editor::EditorComposition composition;
    auto result = composition.registerUiFactory("missing", {});
    assert(!result && result.error().type == editor::Errors::EditorInvalidUiFactory);
    auto profile = composition.registerSceneProfile({});
    assert(!profile && profile.error().type == editor::Errors::SceneProfileInvalid);
    assert(error::ErrorRegistry::instance().find(editor::Errors::EditorRecursiveServiceFactory));
    assert(!error::ErrorRegistry::instance().find(error::errorId("lux.editor.invalid_window_extent")));
    constexpr error::Error failure{editor::Errors::EditorRecursiveServiceFactory};
    static_assert(failure.type == error::errorId("lux.editor.recursive_service_factory"));
}
