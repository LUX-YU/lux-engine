#pragma once

#include <algorithm>
#include <lux/engine/editor/detail/AssetSave.hpp>
#include <lux/engine/editor/detail/TaskResult.hpp>
#include <lux/engine/flowforge/graph/FlowSource.hpp>
#include <lux/engine/flowforge/Compiler.hpp>

namespace lux::editor::flowforge
{
    struct FlowEncoder final
    {
        EditorResult<lux::cxx::SharedBytes<>> operator()(
            const lux::flowforge::FlowSource& capture,
            std::stop_token stop
        ) const noexcept
        {
            if (stop.stop_requested())
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::CANCELLED, "flowforge.encode"});
            }
            auto result = lux::flowforge::encodeFlowSource(capture);
            if (!result)
            {
                return lux::cxx::unexpected(EditorFailure{
                    EEditorError::SOURCE_FAILURE,
                    "flowforge.encode",
                    static_cast<std::uint64_t>(result.error().code),
                    result.error().field,
                    result.error()
                });
            }
            auto owner = std::make_shared<const std::string>(std::move(*result));
            return lux::cxx::SharedBytes<>::fromOwner(owner, std::as_bytes(std::span{owner->data(), owner->size()}));
        }
    };
    struct CompileWork final
    {
        const lux::flowforge::FlowSource* source;
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
                return lux::cxx::unexpected(EditorFailure{
                    EEditorError::SOURCE_FAILURE,
                    "flowforge.materialize",
                    static_cast<std::uint64_t>(graph.error().code),
                    graph.error().field,
                    graph.error()
                });
            }
            lux::flowforge::FlowForgeCompileOptions options;
            options.module_name = "flow_" + uuids::to_string(source->id.uuid());
            std::ranges::replace(options.module_name, '-', '_');
            options.script_abilities = environment.abilities;
            options.script_events = environment.events;
            auto result = lux::flowforge::compileFlowForgeObject(*graph, options);
            if (!result)
            {
                return lux::cxx::unexpected(EditorFailure{
                    EEditorError::SOURCE_FAILURE,
                    "flowforge.compile",
                    static_cast<std::uint64_t>(result.error().code),
                    result.error().message,
                    result.error()
                });
            }
            return std::move(*result);
        }
    };
    struct LinkWork final
    {
        const lux::flowforge::FlowForgeObject* object;
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
                return lux::cxx::unexpected(EditorFailure{
                    EEditorError::SOURCE_FAILURE,
                    "flowforge.link",
                    static_cast<std::uint64_t>(result.error().code),
                    result.error().message,
                    result.error()
                });
            }
            return std::move(*result);
        }
    };
    using FlowCompiled = detail::TCompiledAsset<lux::script::ScriptArtifactAsset, lux::flowforge::FlowSource>;
    struct PackageWork final
    {
        lux::flowforge::FlowSource* source;
        std::shared_ptr<const lux::script::ScriptArtifact> artifact;
        std::string path;
        std::stop_token stop;
        EditorResult<FlowCompiled> operator()() const noexcept
        {
            auto asset = lux::script::ScriptArtifactAsset::create(
                {source->id, lux::script::ScriptArtifactAsset::asset_type},
                artifact
            );
            if (!asset)
            {
                return lux::cxx::unexpected(EditorFailure{
                    EEditorError::SOURCE_FAILURE,
                    "flowforge.artifact",
                    static_cast<std::uint64_t>(asset.error().code),
                    {},
                    asset.error()
                });
            }
            return detail::encodeCompiledAsset(std::move(*asset), std::move(*source), path);
        }
    };
    struct FlowLinkInput final
    {
        lux::flowforge::FlowSource source;
        lux::flowforge::FlowForgeObject object;
        std::string path;
    };
    struct FlowCompilationFailure final
    {
        EditorFailure failure;
        std::optional<FlowLinkInput> retry;
    };
    using FlowCompilationResult = lux::cxx::expected<FlowCompiled, FlowCompilationFailure>;

    template <class Prepared>
    [[nodiscard]] auto finishFlowCompilation(
        Prepared prepared,
        std::filesystem::path linker,
        process::CpuScheduler cpu,
        process::BlockingScheduler blocking,
        process::TaskReporter reporter
    )
    {
        return stdexec::let_value(
            std::move(prepared),
            [linker = std::move(linker), cpu, blocking, reporter](auto& input) mutable noexcept {
                auto linked = stdexec::then(
                    stdexec::schedule(blocking),
                    [&input, linker, reporter]() noexcept -> EditorResult<lux::script::ScriptArtifact> {
                        if (!input)
                            return lux::cxx::unexpected(std::move(input.error()));
                        reporter.setPhase("Link Flow");
                        return LinkWork{&input->object, linker, reporter.stopToken()}();
                    }
                );
                return stdexec::then(
                    stdexec::continues_on(std::move(linked), cpu),
                    [&input,
                     reporter](EditorResult<lux::script::ScriptArtifact> artifact) noexcept -> FlowCompilationResult {
                        if (!artifact)
                        {
                            FlowCompilationFailure failure{std::move(artifact.error())};
                            if (input && !reporter.stopToken().stop_requested())
                                failure.retry.emplace(std::move(*input));
                            return lux::cxx::unexpected(std::move(failure));
                        }
                        reporter.setPhase("Encode Flow");
                        auto compiled = PackageWork{
                            &input->source,
                            std::make_shared<const lux::script::ScriptArtifact>(std::move(*artifact)),
                            input->path,
                            reporter.stopToken()
                        }();
                        if (!compiled)
                            return lux::cxx::unexpected(FlowCompilationFailure{std::move(compiled.error())});
                        return std::move(*compiled);
                    }
                );
            }
        );
    }

    [[nodiscard]] inline auto linkFlowAsset(
        FlowLinkInput input,
        std::filesystem::path linker,
        process::CpuScheduler cpu,
        process::BlockingScheduler blocking,
        process::TaskReporter reporter
    )
    {
        return finishFlowCompilation(
            stdexec::just(lux::cxx::expected<FlowLinkInput, EditorFailure>{std::move(input)}),
            std::move(linker),
            cpu,
            blocking,
            reporter
        );
    }

    [[nodiscard]] inline auto compileFlowAsset(
        lux::flowforge::FlowSource source,
        lux::flowforge::FlowSourceEnvironment environment,
        std::string path,
        std::filesystem::path linker,
        process::CpuScheduler cpu,
        process::BlockingScheduler blocking,
        process::TaskReporter reporter
    )
    {
        auto prepared = stdexec::then(
            stdexec::schedule(cpu),
            [source = std::move(source), environment, path = std::move(path), reporter](
            ) mutable noexcept -> lux::cxx::expected<FlowLinkInput, EditorFailure> {
                reporter.setPhase("Compile Flow");
                auto object = CompileWork{&source, environment, reporter.stopToken()}();
                if (!object)
                    return lux::cxx::unexpected(std::move(object.error()));
                return FlowLinkInput{std::move(source), std::move(*object), std::move(path)};
            }
        );
        return finishFlowCompilation(std::move(prepared), std::move(linker), cpu, blocking, reporter);
    }
}
