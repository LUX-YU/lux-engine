#pragma once

#include <algorithm>
#include <lux/engine/editor/detail/AssetSave.hpp>
#include <lux/engine/editor/detail/TaskResult.hpp>
#include <lux/engine/material/Compiler.hpp>
#include <lux/engine/material/graph/MaterialSource.hpp>
#include <lux/engine/resource/asset/material/MaterialAssets.hpp>

namespace lux::editor::material
{
    struct MaterialEncoder final
    {
        EditorResult<lux::cxx::SharedBytes<>> operator()(
            const lux::material::MaterialSource& capture,
            std::stop_token stop
        ) const noexcept
        {
            if (stop.stop_requested())
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::CANCELLED, "material.encode"});
            }
            auto result = lux::material::encodeMaterialSource(capture);
            if (!result)
            {
                return lux::cxx::unexpected(EditorFailure{
                    EEditorError::SOURCE_FAILURE,
                    "material.encode",
                    static_cast<std::uint64_t>(result.error().code),
                    result.error().field,
                    result.error()
                });
            }
            auto owner = std::make_shared<const std::string>(std::move(*result));
            return lux::cxx::SharedBytes<>::fromOwner(owner, std::as_bytes(std::span{owner->data(), owner->size()}));
        }
    };
    using MaterialCompiled = detail::TCompiledAsset<asset::MaterialAsset, lux::material::MaterialSource>;
    struct CompileWork final
    {
        lux::material::MaterialSource* source;
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
                return lux::cxx::unexpected(EditorFailure{
                    EEditorError::SOURCE_FAILURE,
                    "material.compile",
                    static_cast<std::uint64_t>(result.error().code),
                    result.error().message,
                    std::move(result.error())
                });
            }
            auto material = std::make_shared<const lux::rdesc::MaterialDescription>(std::move(*result));
            auto asset =
                asset::MaterialAsset::create({source->id, asset::MaterialAsset::asset_type}, std::move(material));
            if (!asset)
            {
                return lux::cxx::unexpected(EditorFailure{
                    EEditorError::SOURCE_FAILURE,
                    "material.artifact",
                    static_cast<std::uint64_t>(asset.error().code),
                    {},
                    asset.error()
                });
            }
            return detail::encodeCompiledAsset(std::move(*asset), std::move(*source), path);
        }
    };
    [[nodiscard]] inline auto compileMaterialAsset(
        lux::material::MaterialSource source,
        std::string path,
        process::CpuScheduler cpu,
        process::TaskReporter reporter
    )
    {
        return stdexec::then(
            stdexec::schedule(cpu),
            [source = std::move(source), path = std::move(path), reporter]() mutable noexcept {
                reporter.setPhase("Compile material");
                return CompileWork{&source, path, reporter.stopToken()}();
            }
        );
    }
}
