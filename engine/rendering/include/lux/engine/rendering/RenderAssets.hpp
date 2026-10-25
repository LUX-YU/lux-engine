#pragma once

#include <lux/engine/process/asset_loading/AssetRead.hpp>
#include <lux/engine/render/inputs/ImageShape.hpp>
#include <lux/engine/render/inputs/IndexedTriangleMeshInputs.hpp>
#include <lux/engine/render/inputs/SampledTextureInputs.hpp>
#include <lux/engine/resource/asset/material/MaterialAssets.hpp>

namespace lux::rendering
{
    class RenderExecution;

    struct RenderAssetVersion final
    {
        asset::AssetId asset;
        std::uint64_t revision = 0;
        // Caller-assigned source namespace. Distinguishes project/VFS generations with equal AssetId/revision.
        std::uint64_t source = 0;

        friend bool operator==(const RenderAssetVersion&, const RenderAssetVersion&) noexcept = default;

        friend bool operator<(const RenderAssetVersion& left, const RenderAssetVersion& right) noexcept
        {
            if (left.source != right.source)
            {
                return left.source < right.source;
            }
            return left.asset < right.asset || (left.asset == right.asset && left.revision < right.revision);
        }
    };

    struct RenderAssetLimits final
    {
        std::size_t in_flight = 8;
        // One admitted load owns at most these encoded/decoded limits. The product of admission and
        // decoder bounds limits temporary input, including while Main pauses. This is not GPU residency.
        asset::AssetDecodeLimits decode{64 * 1024 * 1024, 128 * 1024 * 1024, 32};
    };

    struct RenderMesh final
    {
        render::MeshId mesh;
        render::MeshDescription geometry;
    };

    struct RenderTexture final
    {
        render::TextureId texture;
        render::ImageShape description;
    };

    // Main-thread asset/version authority. Reads and decode use the shared ExecutionRuntime;
    // Texture/Mesh transport uses its fixed Asset Client. No Scene/Registry is captured by workers.
    // One acquire/release pair represents one actual Client dependency. Versions may coexist.
    class RenderAssets final
    {
    public:
        // Main-thread fact subscription. Callback records intent only; it may not mutate connections.
        // Scene owns the connection and disconnects before destruction; RenderAssets outlives all Scenes.
        class AvailabilityConnection final
        {
        public:
            AvailabilityConnection(RenderAssets&, void*, void (*)(void*, RenderAssetVersion) noexcept) noexcept;
            ~AvailabilityConnection() noexcept;
            AvailabilityConnection(const AvailabilityConnection&) = delete;
            AvailabilityConnection& operator=(const AvailabilityConnection&) = delete;

        private:
            friend class RenderAssets;
            RenderAssets& assets_;
            AvailabilityConnection* next_{};
            AvailabilityConnection** previous_{};
            void* receiver_;
            void (*completed_)(void*, RenderAssetVersion) noexcept;
        };

        using CreateResult = render::RenderResult<std::unique_ptr<RenderAssets>>;

        // Cold startup: installs raster asset inputs through the ordinary capability API.
        [[nodiscard]] static CreateResult create(
            RenderExecution& rendering,
            process::asset_loading::AssetReadPort source,
            RenderAssetLimits limits = {}
        ) noexcept;

        // close(), continue maintenance frames and wait for settled() before destroying this object.
        // RenderExecution and the file provider outlive all accepted source reads and protocol replies.
        ~RenderAssets() noexcept;
        RenderAssets(const RenderAssets&) = delete;
        RenderAssets& operator=(const RenderAssets&) = delete;

        // An explicit project source is captured by this acquisition, so project candidates and old
        // generations can load concurrently without mutating a shared current-source selector.
        [[nodiscard]] render::RenderResult<void> acquireMesh(
            RenderAssetVersion,
            process::asset_loading::AssetReadPort source = {},
            asset::AssetVfsView snapshot = {}
        ) noexcept;
        [[nodiscard]] render::RenderResult<void> acquireTexture(
            RenderAssetVersion,
            process::asset_loading::AssetReadPort source = {},
            asset::AssetVfsView snapshot = {}
        ) noexcept;
        [[nodiscard]] render::RenderResult<void> acquireMaterial(
            RenderAssetVersion,
            process::asset_loading::AssetReadPort source = {},
            asset::AssetVfsView snapshot = {}
        ) noexcept;
        void releaseMesh(RenderAssetVersion) noexcept;
        void releaseTexture(RenderAssetVersion) noexcept;
        void releaseMaterial(RenderAssetVersion) noexcept;

        // A null pointer means loading or absent. Inspect failure() separately; no query starts work.
        // Borrows end at the next release of that version. Material data is the real Cooked contract.
        [[nodiscard]] const RenderMesh* mesh(RenderAssetVersion) const noexcept;
        [[nodiscard]] const RenderTexture* texture(RenderAssetVersion) const noexcept;
        [[nodiscard]] const rdesc::MaterialDescription* material(RenderAssetVersion) const noexcept;
        [[nodiscard]] render::RenderError failure(RenderAssetVersion) const noexcept;

        // Stops new demands and decoding, not accepted GPU work. Successful late uploads are released
        // through Main frames. Existing Scene material/instance references must already be removed.
        void close() noexcept;
        // No outstanding reads or replies. Failed release records remain owned until final Server cleanup;
        // completion is not success: inspect releaseFailure() before ending the rendering session.
        [[nodiscard]] bool settled() const noexcept;
        [[nodiscard]] render::RenderError releaseFailure() const noexcept;

    private:
        struct Impl;
        explicit RenderAssets(std::unique_ptr<Impl>) noexcept;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::rendering
