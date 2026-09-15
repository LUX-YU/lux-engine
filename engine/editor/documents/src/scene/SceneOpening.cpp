#include <lux/engine/editor/scene/SceneEditor.hpp>
#include <lux/engine/editor/scene/NativeScene.hpp>
#include <lux/engine/editor/rendering/EditorRenderer.hpp>
#include <fstream>
#include <atomic>

namespace lux::editor::scene
{
    namespace
    {
        class SceneOpening final : public DocumentOpening
        {
            struct Closed final
            {
                using receiver_concept = stdexec::receiver_t;
                SceneOpening *owner;

                stdexec::empty_env get_env() const noexcept
                {
                    return {};
                }

                void set_value() && noexcept
                {
                    owner->closed_.store(true, std::memory_order_release);
                }
            };

            using CloseOperation = stdexec::connect_result_t<process::TaskScopeCloseSender, Closed>;

          public:
            SceneOpening(Project &project, lux::asset::AssetId source, std::filesystem::path path,
                         process::ExecutionRuntime &runtime, rendering::EditorRenderer &renderer,
                         std::shared_ptr<const lux::scene::SceneMetaManager> metadata)
                : project_(project), source_(source), path_(std::move(path)), renderer_(renderer),
                  metadata_(std::move(metadata))
            {
                auto read = stdexec::then(
                    stdexec::schedule(*runtime.blocking()),
                    [this]() noexcept
                    {
                        std::error_code error;
                        const auto size = std::filesystem::file_size(path_, error);
                        if (error || size > 256U * 1024U * 1024U)
                        {
                            failure_ = EditorFailure{EEditorError::SOURCE_FAILURE, "scene.read",
                                                     static_cast<std::uint64_t>(error.value()), path_.string()};
                            return;
                        }
                        auto data = std::make_shared<std::vector<std::byte>>(static_cast<std::size_t>(size));
                        std::ifstream file(path_, std::ios::binary);
                        if (!file.read(reinterpret_cast<char *>(data->data()), static_cast<std::streamsize>(size)))
                        {
                            failure_ = EditorFailure{EEditorError::SOURCE_FAILURE, "scene.read", 0, path_.string()};
                            return;
                        }
                        bytes_ = lux::cxx::SharedBytes<>::fromOwner(data, *data);
                    });
                auto cpu = stdexec::continues_on(std::move(read), runtime.cpu());
                auto decode = stdexec::then(std::move(cpu),
                                            [this]() noexcept
                                            {
                                                if (std::holds_alternative<std::monostate>(failure_))
                                                {
                                                    content_ = decodeNativeScene(bytes_, stop_.get_token());
                                                }
                                                ready_.store(true, std::memory_order_release);
                                            });
                auto errors = stdexec::upon_error(std::move(decode),
                                                  [this](process::EExecutionError error) noexcept
                                                  {
                                                      failure_ = EditorFailure{EEditorError::EXECUTION_FAILURE,
                                                                               "process.schedule",
                                                                               static_cast<std::uint64_t>(error)};
                                                      ready_.store(true, std::memory_order_release);
                                                  });
                auto stopped = stdexec::upon_stopped(std::move(errors),
                                                     [this]() noexcept
                                                     {
                                                         failure_ =
                                                             EditorFailure{EEditorError::CANCELLED, "scene.open"};
                                                         ready_.store(true, std::memory_order_release);
                                                     });
                const auto started = tasks_.start(std::move(stopped));
                if (!started)
                {
                    failure_ = EditorFailure{EEditorError::EXECUTION_FAILURE, "process.task",
                                             static_cast<std::uint64_t>(started.error())};
                    ready_.store(true, std::memory_order_release);
                }
            }

            void cancel() noexcept override
            {
                stop_.request_stop();
                tasks_.requestStop();
            }

            void poll() override
            {
                if (ready_.load(std::memory_order_acquire) && !close_)
                {
                    close_.reset(new CloseOperation(stdexec::connect(tasks_.close(), Closed{this})));
                    stdexec::start(*close_);
                }
            }

            bool settled() const noexcept override
            {
                return closed_.load(std::memory_order_acquire);
            }

            EditorResult<std::unique_ptr<DocumentEditor>> take() override
            {
                if (const auto *error = std::get_if<EditorFailure>(&failure_))
                {
                    return lux::cxx::unexpected(*error);
                }
                if (!content_)
                {
                    return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "scene.codec",
                                                              static_cast<std::uint64_t>(content_.error().code),
                                                              path_.string(), content_.error()});
                }
                if (stop_.stop_requested())
                {
                    return lux::cxx::unexpected(EditorFailure{EEditorError::CANCELLED, "scene.open"});
                }
                if (content_->scene->id() != source_)
                {
                    return lux::cxx::unexpected(
                        EditorFailure{EEditorError::SOURCE_FAILURE, "scene.identity", 0,
                                      "The scene package identity differs from its project entry"});
                }
                auto result = SceneEditor::open(*content_, project_, renderer_, metadata_);
                if (!result)
                {
                    return lux::cxx::unexpected(result.error());
                }
                return std::unique_ptr<DocumentEditor>(std::move(*result));
            }

          private:
            Project &project_;
            lux::asset::AssetId source_;
            std::filesystem::path path_;
            rendering::EditorRenderer &renderer_;
            std::shared_ptr<const lux::scene::SceneMetaManager> metadata_;
            lux::cxx::SharedBytes<> bytes_;
            lux::cxx::expected<NativeScene, NativeSceneFailure> content_{
                lux::cxx::unexpected(NativeSceneFailure{ENativeSceneError::CANCELLED})};
            std::variant<std::monostate, EditorFailure> failure_;
            std::stop_source stop_;
            std::atomic<bool> ready_{}, closed_{};
            process::TaskScope tasks_;
            std::unique_ptr<CloseOperation> close_;
        };
    } // namespace

    EditorResult<std::unique_ptr<DocumentOpening>> openSceneDocument(
        Project &project, const OpenDocumentRequest &request, process::ExecutionRuntime &runtime,
        rendering::EditorRenderer &renderer, std::shared_ptr<const lux::scene::SceneMetaManager> metadata)
    {
        const auto *asset = project.asset(request.key.source);
        if (!asset || asset->kind != EProjectAssetKind::SCENE || !runtime.blocking())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "scene.open"});
        }
        return std::unique_ptr<DocumentOpening>(new SceneOpening(
            project, request.key.source, project.root() / asset->source_path, runtime, renderer, std::move(metadata)));
    }
} // namespace lux::editor::scene
