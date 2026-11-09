#pragma once
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/resource/identity/AssetId.hpp>
#include <memory>
#include <string>

namespace lux::scene
{
    struct MeshQueryGeometry;
}

namespace lux::editor
{
    enum class EMeshQueryAssetError : std::uint8_t
    {
        CANCELLED,
        READ,
        BUILD,
        EXECUTION
    };

    struct MeshQueryAssetFailure final
    {
        EMeshQueryAssetError code{};
        std::string detail;
    };
    template <class T> using MeshQueryAssetResult = cxx::expected<T, MeshQueryAssetFailure>;

    class EditorContext;
    class ProjectAssets;

    // Professional, project-scoped immutable Mesh/BVH cache and Scene query subscriptions.
    // Loads use the shared CPU/IO resources; a Scene participant disconnects before destruction.
    class MeshQueryAssets final : public object::LuxObject
    {
    public:
        MeshQueryAssets(EditorContext&, ProjectAssets&) noexcept;
        ~MeshQueryAssets() noexcept;
        [[nodiscard]] MeshQueryAssetResult<void> prepare(asset::AssetId) noexcept;
        [[nodiscard]] std::shared_ptr<const scene::MeshQueryGeometry> geometry(asset::AssetId) const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor
