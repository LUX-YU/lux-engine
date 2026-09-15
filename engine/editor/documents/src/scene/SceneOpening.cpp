#include <lux/engine/editor/DocumentSource.hpp>
#include <lux/engine/editor/scene/NativeScene.hpp>
#include <lux/engine/editor/scene/SceneEditor.hpp>

namespace lux::editor::scene
{
namespace
{
struct SceneCodec final
{
    using Source = NativeScene;
    static constexpr std::size_t max_bytes = 256U * 1024U * 1024U;
    rendering::EditorRenderer &renderer;
    std::shared_ptr<const lux::scene::SceneMetaManager> metadata;

    static lux::asset::AssetId identity(const Source &source) noexcept
    {
        return source.scene->id();
    }
    static EditorResult<Source> decode(const lux::cxx::SharedBytes<> &bytes, std::stop_token stop)
    {
        auto source = decodeNativeScene(bytes, stop);
        if (!source)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE,
                                                      "scene.codec",
                                                      static_cast<std::uint64_t>(source.error().code),
                                                      {},
                                                      source.error()});
        }
        return std::move(*source);
    }
    EditorResult<std::unique_ptr<DocumentEditor>> adopt(Source &source, Project &project,
                                                        process::ExecutionRuntime &runtime) const
    {
        auto result = SceneEditor::open(source, project, runtime, renderer, metadata);
        if (!result)
        {
            return lux::cxx::unexpected(result.error());
        }
        return std::unique_ptr<DocumentEditor>(std::move(*result));
    }
};
} // namespace

EditorResult<std::unique_ptr<DocumentOpening>> openSceneDocument(
    Project &project, const OpenDocumentRequest &request, process::ExecutionRuntime &runtime,
    rendering::EditorRenderer &renderer, std::shared_ptr<const lux::scene::SceneMetaManager> metadata)
{
    const auto *entry = project.asset(request.key.source);
    const bool valid_target = request.key.project == project.manifest().id && request.key.type == kSceneDocumentType;
    if (!entry || entry->kind != EProjectAssetKind::SCENE || !runtime.blocking() || !metadata || !valid_target)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "scene.open"});
    }
    return std::unique_ptr<DocumentOpening>(
        new lux::editor::detail::SourceOpening<SceneCodec>(project, *entry, runtime,
                                                         SceneCodec{renderer, std::move(metadata)}));
}
} // namespace lux::editor::scene
