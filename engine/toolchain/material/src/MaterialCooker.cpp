#include <lux/engine/material/Cooker.hpp>

#include <lux/engine/material/import/MaterialToGraph.hpp>

#include <new>
#include <utility>

namespace lux::material
{
    namespace
    {
        [[nodiscard]] MaterialCookFailure failure(
            EMaterialCookError code,
            std::string message,
            std::optional<MaterialCompileFailure> compile_failure = std::nullopt
        ) noexcept
        {
            return MaterialCookFailure{code, std::move(message), std::move(compile_failure)};
        }
    } // namespace

    lux::cxx::expected<std::shared_ptr<const lux::asset::MaterialAsset>, MaterialCookFailure>
    cookMaterial(lux::asset::AssetInfo info, const MaterialGraph& graph) noexcept
    {
        if (info.id.isNull())
            return lux::cxx::unexpected(failure(EMaterialCookError::INVALID_ASSET_INFO,
                                                 "material AssetInfo has a null AssetId"));

        auto compiled = compileMaterial(graph);
        if (!compiled)
        {
            auto compile_failure = std::move(compiled.error());
            auto message = compile_failure.message;
            return lux::cxx::unexpected(failure(EMaterialCookError::COMPILE_FAILURE, std::move(message),
                                                 std::move(compile_failure)));
        }

        try
        {
            auto description = std::make_shared<const lux::rdesc::MaterialDescription>(std::move(*compiled));
            auto asset = lux::asset::MaterialAsset::create(std::move(info), std::move(description));
            if (!asset)
                return lux::cxx::unexpected(failure(EMaterialCookError::INVALID_ASSET,
                                                     "compiled MaterialDescription failed typed Asset validation"));
            return *asset;
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(EMaterialCookError::ALLOCATION_FAILURE, "allocation failure"));
        }
    }

    lux::cxx::expected<std::shared_ptr<const lux::asset::MaterialAsset>, MaterialCookFailure>
    cookImportedMaterial(
        lux::asset::AssetInfo info,
        const ImportedMaterialDescription& imported
    ) noexcept
    {
        try
        {
            auto graph = compiler::materialToGraph(imported);
            if (!graph)
                return lux::cxx::unexpected(failure(EMaterialCookError::IMPORT_FAILURE,
                                                     std::move(graph.error())));
            return cookMaterial(std::move(info), *graph);
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(EMaterialCookError::ALLOCATION_FAILURE, "allocation failure"));
        }
    }
} // namespace lux::material
