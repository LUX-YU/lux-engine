#pragma once

#include <algorithm>
#include <lux/engine/editor/detail/DocumentSave.hpp>
#include <lux/engine/editor/flowforge/FlowForgeEditor.hpp>
#include <lux/engine/flowforge/Compiler.hpp>

namespace lux::editor::flowforge
{
    struct FlowEncoder final
    {
        EditorResult<lux::cxx::SharedBytes<>> operator()(const lux::flowforge::FlowSourceDocument &capture,
                                                         std::stop_token stop) const noexcept
        {
            if (stop.stop_requested())
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::CANCELLED, "flowforge.encode"});
            }
            auto result = lux::flowforge::encodeFlowSource(capture);
            if (!result)
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "flowforge.encode",
                                                          static_cast<std::uint64_t>(result.error().code),
                                                          result.error().field, result.error()});
            }
            auto owner = std::make_shared<const std::string>(std::move(*result));
            return lux::cxx::SharedBytes<>::fromOwner(owner, std::as_bytes(std::span{owner->data(), owner->size()}));
        }
    };
    struct CompileWork final
    {
        const lux::flowforge::FlowSourceDocument *source;
        lux::flowforge::FlowSourceEnvironment environment;
        std::stop_token stop;
        EditorResult<lux::flowforge::FlowForgeObject> operator()() const noexcept
        {
            if (stop.stop_requested())
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::CANCELLED, "flowforge.compile"});
            }
            auto graph = lux::flowforge::materializeFlowSource(*source, environment);
            if (!graph)
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "flowforge.materialize",
                                                          static_cast<std::uint64_t>(graph.error().code),
                                                          graph.error().field, graph.error()});
            }
            lux::flowforge::FlowForgeCompileOptions options;
            options.module_name = "flow_" + uuids::to_string(source->id.uuid());
            std::ranges::replace(options.module_name, '-', '_');
            options.script_abilities = environment.abilities;
            options.script_events = environment.events;
            auto result = lux::flowforge::compileFlowForgeObject(*graph, options);
            if (!result)
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "flowforge.compile",
                                                          static_cast<std::uint64_t>(result.error().code),
                                                          result.error().message, result.error()});
            }
            return std::move(*result);
        }
    };
    struct LinkWork final
    {
        const lux::flowforge::FlowForgeObject *object;
        std::filesystem::path linker;
        std::stop_token stop;
        EditorResult<lux::script::ScriptArtifact> operator()() const noexcept
        {
            if (stop.stop_requested())
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::CANCELLED, "flowforge.link"});
            }
            auto result = lux::flowforge::linkFlowForgeObject(*object, linker);
            if (!result)
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "flowforge.link",
                                                          static_cast<std::uint64_t>(result.error().code),
                                                          result.error().message, result.error()});
            }
            return std::move(*result);
        }
    };
    using FlowCompiled = detail::CompiledDocument<lux::script::ScriptArtifactAsset>;
    struct PackageWork final
    {
        const lux::flowforge::FlowSourceDocument *source;
        std::shared_ptr<const lux::script::ScriptArtifact> artifact;
        std::string path;
        std::stop_token stop;
        EditorResult<FlowCompiled> operator()() const noexcept
        {
            auto source_bytes = FlowEncoder{}(*source, stop);
            if (!source_bytes)
            {
                return lux::cxx::unexpected(source_bytes.error());
            }
            auto asset = lux::script::ScriptArtifactAsset::create(
                {source->id, lux::script::ScriptArtifactAsset::asset_type}, artifact);
            if (!asset)
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE,
                                                          "flowforge.artifact",
                                                          static_cast<std::uint64_t>(asset.error().code),
                                                          {},
                                                          asset.error()});
            }
            return detail::encodeCompiledDocument(std::move(*asset), std::move(*source_bytes), path);
        }
    };
    struct FlowCompilation final
    {
        using Compiling = detail::ScheduledDocumentTask<process::CpuScheduler, CompileWork>;
        using Linking = detail::ScheduledDocumentTask<process::BlockingScheduler, LinkWork>;
        using Packaging = detail::ScheduledDocumentTask<process::CpuScheduler, PackageWork>;
        FlowCompilation(FlowCompileId request, const editing::HistorySnapshot &history,
                        lux::flowforge::FlowSourceDocument source, lux::flowforge::FlowSourceEnvironment environment,
                        process::ExecutionRuntime &execution, std::filesystem::path linker_path,
                        std::string package_path)
            : id(request), state(history.current), revision(history.revision), capture(std::move(source)),
              runtime(execution), linker(std::move(linker_path)), path(std::move(package_path))
        {
            auto &task =
                work.emplace<Compiling>(runtime, stdexec::then(stdexec::schedule(runtime.cpu()),
                                                               CompileWork{&capture, environment, stop.get_token()}));
            task.start();
        }
        bool settled() const noexcept
        {
            return work.index() == 0;
        }
        void startLink()
        {
            status = FlowCompilePending{EFlowCompileStage::LINKING};
            auto &task = work.emplace<Linking>(
                runtime,
                stdexec::then(stdexec::schedule(*runtime.blocking()),
                              LinkWork{&std::get<lux::flowforge::FlowForgeObject>(output), linker, stop.get_token()}));
            task.start();
        }
        bool poll()
        {
            if (auto *task = std::get_if<Compiling>(&work); task && task->ready())
            {
                auto result = task->take();
                work.emplace<std::monostate>();
                if (!result)
                {
                    status = FlowCompileFailed{state, revision, std::move(result.error()), false};
                    return true;
                }
                output.emplace<lux::flowforge::FlowForgeObject>(std::move(*result));
                startLink();
            }
            if (auto *task = std::get_if<Linking>(&work); task && task->ready())
            {
                auto result = task->take();
                work.emplace<std::monostate>();
                if (!result)
                {
                    status = FlowCompileFailed{state, revision, std::move(result.error()), !stop.stop_requested()};
                }
                else
                {
                    auto artifact = std::make_shared<const lux::script::ScriptArtifact>(std::move(*result));
                    status = FlowCompilePending{EFlowCompileStage::PACKAGING};
                    work.emplace<Packaging>(
                            runtime, stdexec::then(stdexec::schedule(runtime.cpu()),
                                                   PackageWork{&capture, std::move(artifact), path, stop.get_token()}))
                        .start();
                    return false;
                }
                return true;
            }
            if (auto *task = std::get_if<Packaging>(&work); task && task->ready())
            {
                auto result = task->take();
                work.emplace<std::monostate>();
                if (!result)
                {
                    status = FlowCompileFailed{state, revision, std::move(result.error()), false};
                }
                else
                {
                    output.emplace<FlowCompiled>(std::move(*result));
                    status = FlowCompileSucceeded{state, revision, true};
                }
                return true;
            }
            return false;
        }
        FlowCompileId id;
        editing::StateId state;
        editing::Revision revision;
        lux::flowforge::FlowSourceDocument capture;
        process::ExecutionRuntime &runtime;
        std::filesystem::path linker;
        std::string path;
        std::stop_source stop;
        std::variant<std::monostate, lux::flowforge::FlowForgeObject, FlowCompiled> output;
        std::variant<std::monostate, Compiling, Linking, Packaging> work;
        FlowCompileStatus status{FlowCompilePending{}};
    };

} // namespace lux::editor::flowforge
