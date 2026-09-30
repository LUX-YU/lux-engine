#include <lux/engine/editor/detail/EditorContextStartup.hpp>
#include <lux/engine/editor/detail/TaskResult.hpp>
#include <lux/engine/editor/detail/EditorImpl.hpp>
#include <algorithm>
#include <lux/engine/EngineRendering.hpp>
#include <lux/engine/ui/rendering/RenderFeature.hpp>
#include <lux/engine/editor/ui/ComponentEditors.hpp>
#include <fstream>
#include <exception>
#include <thread>

namespace lux::editor
{
    EditorResult<std::unique_ptr<Editor>> Editor::create(EditorConfig config, Editor::Assembly assemble) noexcept
    {
        return Impl::create(std::move(config), assemble);
    }

    EditorResult<std::unique_ptr<Editor>> Editor::Impl::create(EditorConfig config, Editor::Assembly assemble) noexcept
    {
        const auto& spec = config.window;
        const bool invalid_window =
            !spec.width || !spec.height || spec.width > INT_MAX || spec.height > INT_MAX || spec.title.empty();
        if (config.project_file.empty() || invalid_window)
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "editor.config"});
        std::unique_ptr<Impl> prepared;
        std::unique_ptr<Editor> editor;
        EditorResult<void> status;
        try
        {
            prepared = std::make_unique<Impl>();
            if (!prepared->platform.valid())
                return lux::cxx::unexpected(EditorFailure{EEditorError::FRONTEND_FAILURE, "window.initialize"});
            prepared->window = std::make_unique<window::LuxWindow>(int(spec.width), int(spec.height), spec.title);
            if (!prepared->window->isInitialized())
                return lux::cxx::unexpected(EditorFailure{EEditorError::FRONTEND_FAILURE, "window.create"});
            prepared->window->hide(!spec.visible);
            detail::EditorContextCreateInfo info;
            info.project_file = config.project_file;
            info.plugin_root = config.plugin_root;
            info.product_features = {render::kUiRenderRenderFeatureRegistration};
            info.product_editors = ui::componentEditors();
            auto application = engine::EngineContext::create(config.execution, {0, 1024});
            if (!application)
                return lux::cxx::unexpected(
                    EditorFailure{EEditorError::EXECUTION_FAILURE, "editor.execution", 0, {}, application.error()}
                );
            prepared->engine = std::move(*application);
            auto queue = object::ObjectMessageQueue::create(64);
            if (!queue)
            {
                if (queue.error() == object::EObjectQueueError::ALLOCATION_FAILURE)
                    std::terminate();
                return lux::cxx::unexpected(
                    EditorFailure{EEditorError::CAPACITY, "editor.messages", 0, {}, queue.error()}
                );
            }
            prepared->messages.emplace(std::move(*queue));
            prepared->messages->setWake(&window::LuxWindow::wakeEvents);
            prepared->engine->execution().setWake(&window::LuxWindow::wakeEvents);
            auto renderer =
                engine::initializeRendering(*prepared->engine, window::LuxWindow::requiredVulkanInstanceExtensions());
            if (!renderer)
                status = lux::cxx::unexpected(
                    EditorFailure{EEditorError::FRONTEND_FAILURE, "editor.render", 0, {}, renderer.error()}
                );
            else
            {
                editor = std::unique_ptr<Editor>(new Editor(std::move(config), std::move(prepared)));
                status = detail::EditorContextAccess::create(
                    editor->impl_->context,
                    *editor,
                    *editor->impl_->engine,
                    *editor->impl_->messages,
                    std::move(info)
                );
            }
            if (status)
            {
                editor->impl_->project_ = &editor->impl_->context->project();
                editor->impl_->window->on_close = [owner = editor.get()](const window::WindowCloseEvent&) {
                    owner->requestExit();
                };
                auto& process = editor->impl_->context->execution();
                lux::ui::FontSource font;
                if (editor->impl_->config_.window.font)
                {
                    auto read =
                        [spec = *editor->impl_->config_.window.font]() noexcept -> EditorResult<lux::ui::FontSource> {
                        {
                            std::ifstream input(spec.file, std::ios::binary | std::ios::ate);
                            const auto size = input ? std::streamoff(input.tellg()) : -1;
                            if (size <= 0 || size > 32 * 1024 * 1024)
                                return lux::cxx::unexpected(
                                    EditorFailure{EEditorError::SOURCE_FAILURE, "editor.font.read"}
                                );
                            lux::ui::FontSource result;
                            result.bytes.resize(std::size_t(size));
                            result.ranges = spec.ranges;
                            result.face = spec.face;
                            result.size_pixels = spec.size_pixels;
                            input.seekg(0);
                            if (!input.read(reinterpret_cast<char*>(result.bytes.data()), size))
                                return lux::cxx::unexpected(
                                    EditorFailure{EEditorError::SOURCE_FAILURE, "editor.font.read"}
                                );
                            return result;
                        }
                    };
                    std::optional<EditorResult<lux::ui::FontSource>> loaded;
                    process::TaskScope tasks(process);
                    auto admitted = tasks.submit(
                        {"Read editor font", "Startup"},
                        [scheduler = *process.blocking(), read = std::move(read)](process::TaskReporter
                        ) mutable noexcept { return stdexec::then(stdexec::schedule(scheduler), std::move(read)); },
                        [&loaded](auto&& result) noexcept { loaded.emplace(detail::taskResult(std::move(result))); }
                    );
                    if (!admitted)
                        status = lux::cxx::unexpected(EditorFailure{
                            EEditorError::EXECUTION_FAILURE,
                            "editor.font.submit",
                            0,
                            {},
                            admitted.error()
                        });
                    else
                    {
                        if (!process.waitUntil([&]() noexcept { return loaded.has_value(); }))
                            std::terminate();
                        if (*loaded)
                            font = std::move(**loaded);
                        else
                            status = lux::cxx::unexpected(std::move(loaded->error()));
                    }
                }
                if (status)
                    status = editor->impl_->startDesktop(process, editor->impl_->config_.window.font ? &font : nullptr);
                if (status && assemble)
                    status = assemble(*editor, *editor->impl_->context);
                if (status)
                {
                    editor->impl_->initializeMenu();
                    editor->impl_->startWorkspace();
                }
            }
        }
        catch (const std::bad_alloc&)
        {
            std::terminate();
        }
        catch (...)
        {
            status = lux::cxx::unexpected(EditorFailure{EEditorError::FRONTEND_FAILURE, "editor.create"});
        }
        if (status)
            return editor;
        return lux::cxx::unexpected(std::move(status.error()));
    }

    EditorResult<void> Editor::Impl::startDesktop(process::ExecutionRuntime& process, const lux::ui::FontSource* font)
    {
        auto cpu_ui = root->initialize({.font = font});
        if (!cpu_ui)
            return lux::cxx::unexpected(
                EditorFailure{EEditorError::FRONTEND_FAILURE, "editor.ui.initialize", 0, {}, cpu_ui.error()}
            );
        root->bindWindow(window.get());
        auto created = desktop::Presentation::create(
            *root,
            process,
            engine->sceneRuntime(),
            engine->renderContext()->runtime(),
            engine->renderContext()->resources(),
            window.get()
        );
        if (!created)
            return lux::cxx::unexpected(
                EditorFailure{EEditorError::FRONTEND_FAILURE, "desktop.presentation", 0, {}, created.error()}
            );
        presentation = std::move(*created);
        return {};
    }
} // namespace lux::editor
