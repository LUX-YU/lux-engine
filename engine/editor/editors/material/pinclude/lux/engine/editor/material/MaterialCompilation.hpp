#pragma once

#include <algorithm>
#include <lux/engine/editor/detail/DocumentSave.hpp>
#include <lux/engine/editor/material/MaterialEditor.hpp>
#include <lux/engine/material/Compiler.hpp>
#include <lux/engine/resource/asset/material/MaterialAssets.hpp>

namespace lux::editor::material
{
    struct MaterialEncoder final
    {
        EditorResult<lux::cxx::SharedBytes<>> operator()(const lux::material::MaterialSourceDocument &capture,
                                                         std::stop_token stop) const noexcept
        {
            if (stop.stop_requested())
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::CANCELLED, "material.encode"});
            }
            auto result = lux::material::encodeMaterialSource(capture);
            if (!result)
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "material.encode",
                                                          static_cast<std::uint64_t>(result.error().code),
                                                          result.error().field, result.error()});
            }
            auto owner = std::make_shared<const std::string>(std::move(*result));
            return lux::cxx::SharedBytes<>::fromOwner(owner, std::as_bytes(std::span{owner->data(), owner->size()}));
        }
    };
    using MaterialCompiled = detail::CompiledDocument<asset::MaterialAsset>;
    struct CompileWork final
    {
        const lux::material::MaterialSourceDocument *source;
        std::string path;
        std::stop_token stop;
        EditorResult<MaterialCompiled> operator()() const noexcept
        {
            if (stop.stop_requested())
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::CANCELLED, "material.compile"});
            }
            auto result = lux::material::compileMaterial(source->graph);
            if (!result)
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "material.compile",
                                                          static_cast<std::uint64_t>(result.error().code),
                                                          result.error().message, std::move(result.error())});
            }
            auto source_bytes = MaterialEncoder{}(*source, stop);
            if (!source_bytes)
            {
                return lux::cxx::unexpected(source_bytes.error());
            }
            auto material = std::make_shared<const lux::rdesc::MaterialDescription>(std::move(*result));
            auto asset =
                asset::MaterialAsset::create({source->id, asset::MaterialAsset::asset_type}, std::move(material));
            if (!asset)
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE,
                                                          "material.artifact",
                                                          static_cast<std::uint64_t>(asset.error().code),
                                                          {},
                                                          asset.error()});
            }
            return detail::encodeCompiledDocument(std::move(*asset), std::move(*source_bytes), path);
        }
    };
    struct MaterialCompilation final
    {
        MaterialCompilation(MaterialCompileId request, const editing::HistorySnapshot &history,
                            const lux::material::MaterialSourceDocument &source, std::string path,
                            process::ExecutionRuntime &runtime)
            : id(request), state(history.current), revision(history.revision),
              capture{source.id, source.name, source.graph.clone()},
              task(runtime, stdexec::then(stdexec::schedule(runtime.cpu()),
                                          CompileWork{&capture, std::move(path), stop.get_token()}))
        {
            task.start();
        }
        MaterialCompileId id;
        editing::StateId state;
        editing::Revision revision;
        lux::material::MaterialSourceDocument capture;
        std::stop_source stop;
        detail::ScheduledDocumentTask<process::CpuScheduler, CompileWork> task;
        MaterialCompileStatus status{MaterialCompilePending{}};
        std::variant<std::monostate, MaterialCompiled> output;
    };

} // namespace lux::editor::material
