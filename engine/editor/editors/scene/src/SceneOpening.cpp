#include <lux/engine/editor/Editor.hpp>
#include <lux/engine/editor/detail/DocumentSource.hpp>
#include <lux/engine/editor/rendering/EditorRenderer.hpp>
#include <lux/engine/editor/scene/NativeScene.hpp>
#include <lux/engine/editor/scene/SceneEditor.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/SceneRenderBinding.hpp>

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
    std::shared_ptr<detail::SceneRunSlot> run_slot;

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
    std::unique_ptr<lux::scene::SceneRenderBinding> binding;
    std::unique_ptr<SceneEditor> document;
    std::variant<std::monostate, EditorFailure> failure;
    bool initialized{};

    EditorResult<bool> prepare(Source &source, Project &project, process::ExecutionRuntime &runtime,
                               std::stop_token stop)
    {
        using State = lux::scene::ESceneRenderBindingState;
        if (stop.stop_requested())
        {
            failure = EditorFailure{EEditorError::CANCELLED, "scene.open"};
        }
        if (!initialized && failure.index() == 0)
        {
            initialized = true;
            const auto &description = source.scene->data();
            for (std::size_t index{}; index < description.systemCount(); ++index)
            {
                const auto system = description.systemAt(index);
                if (system.type() != lux::scene::builtinRenderSystemRegistration().type)
                {
                    continue;
                }
                auto started = lux::scene::SceneRenderBinding::begin(renderer, system, metadata);
                if (!started)
                {
                    failure = EditorFailure{EEditorError::SOURCE_FAILURE,
                                            "scene.render.begin",
                                            static_cast<std::uint64_t>(started.error().scene.code),
                                            {},
                                            started.error()};
                    break;
                }
                binding = std::move(*started);
                break;
            }
            if (!binding && failure.index() == 0)
            {
                auto opened =
                    SceneEditor::open(source, project, runtime, renderer, metadata, nullptr, binding, run_slot);
                if (!opened)
                {
                    failure = opened.error();
                }
                else
                {
                    document = std::move(*opened);
                }
            }
        }
        if (binding)
        {
            binding->poll(4);
            if (binding->state() == State::FAILED && failure.index() == 0)
            {
                failure = EditorFailure{EEditorError::SOURCE_FAILURE,
                                        "scene.render.prepare",
                                        static_cast<std::uint64_t>(binding->failure().scene.code),
                                        {},
                                        binding->failure()};
            }
            if (failure.index() == 0 && binding->state() == State::READY)
            {
                auto input = binding->takeInput();
                if (!input)
                {
                    failure = EditorFailure{EEditorError::SOURCE_FAILURE,
                                            "scene.render.input",
                                            static_cast<std::uint64_t>(input.error().scene.code),
                                            {},
                                            input.error()};
                }
                else
                {
                    auto opened =
                        SceneEditor::open(source, project, runtime, renderer, metadata, &*input, binding, run_slot);
                    if (!opened)
                    {
                        failure = opened.error();
                    }
                    else
                    {
                        document = std::move(*opened);
                    }
                }
            }
        }
        if (failure.index() != 0)
        {
            if (document)
            {
                document->requestClose();
                PollBudget budget;
                document->poll(budget);
                if (document->closeStatus().state != ECloseState::CLOSED)
                {
                    return false;
                }
                document.reset();
            }
            if (binding)
            {
                binding->requestClose();
                binding->poll(4);
                if (binding->state() != State::CLOSED)
                {
                    return false;
                }
                binding.reset();
            }
            return lux::cxx::unexpected(std::move(std::get<EditorFailure>(failure)));
        }
        return document != nullptr;
    }

    EditorResult<std::unique_ptr<DocumentEditor>> adopt(Source &, Project &, process::ExecutionRuntime &)
    {
        return std::unique_ptr<DocumentEditor>(std::move(document));
    }
};
} // namespace

DocumentRegistration sceneDocumentRegistration(process::ExecutionRuntime &runtime, rendering::EditorRenderer &renderer,
                                               std::shared_ptr<const lux::scene::SceneMetaManager> metadata)
{
    return {std::string(kSceneDocumentType),
            [&runtime, &renderer, metadata = std::move(metadata), run_slot = std::make_shared<detail::SceneRunSlot>()](
                Project &project, const OpenDocumentRequest &request) -> EditorResult<std::unique_ptr<DocumentOpening>>
            {
                const auto *entry = project.asset(request.key.source);
                const bool valid =
                    request.key.project == project.manifest().id && request.key.type == kSceneDocumentType;
                if (!entry || entry->kind != EProjectAssetKind::SCENE || !runtime.blocking() || !metadata || !valid)
                {
                    return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "scene.open"});
                }
                return std::unique_ptr<DocumentOpening>(new lux::editor::detail::SourceOpening<SceneCodec>(
                    project, *entry, runtime, SceneCodec{renderer, metadata, run_slot}));
            }};
}
} // namespace lux::editor::scene
