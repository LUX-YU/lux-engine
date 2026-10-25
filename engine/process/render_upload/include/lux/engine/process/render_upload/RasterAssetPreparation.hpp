#pragma once

#include <lux/engine/description/Mesh.hpp>
#include <lux/engine/process/asset_loading/AssetLoadSender.hpp>
#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/inputs/IndexedTriangleMeshInputs.hpp>

namespace lux::process::render_upload
{
    struct PackedMesh final
    {
        render::MeshDescription description;
        std::vector<std::byte> bytes;
    };

    [[nodiscard]] PackedMesh packMesh(const rdesc::Mesh&) noexcept;
    [[nodiscard]] render::RenderError assetLoadError(const asset_loading::AssetLoadFailure&) noexcept;
    [[nodiscard]] render::RenderResult<void> registerAssetErrors() noexcept;

    // Decoding stays on the shared CPU scheduler; the caller selects delivery and upload affinity.
    template <class Asset>
    auto decodeAsset(
        asset_loading::AssetReadPort source,
        execution::CpuScheduler cpu,
        asset::AssetId id,
        asset::AssetDecodeLimits limits,
        std::stop_token stop,
        asset::AssetVfsView snapshot
    ) noexcept
    {
        using Result = render::RenderResult<std::shared_ptr<const Asset>>;
        return asset_loading::loadAsset<Asset>(std::move(source), cpu, id, limits, stop, std::move(snapshot)) |
               stdexec::then([](auto value) noexcept -> Result { return std::move(value); }) |
               stdexec::upon_error(
                   [](asset_loading::AssetLoadFailure failure) noexcept -> Result
                   { return cxx::unexpected(assetLoadError(failure)); }
               ) |
               stdexec::upon_stopped(
                   []() noexcept -> Result { return cxx::unexpected(render::RenderError{render::kCommandCancelled}); }
               );
    }
} // namespace lux::process::render_upload
