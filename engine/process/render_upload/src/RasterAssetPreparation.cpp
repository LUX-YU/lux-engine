#include <Eigen/Geometry>
#include <cstring>
#include <lux/engine/error/ErrorRegistry.hpp>
#include <lux/engine/process/render_upload/RasterAssetPreparation.hpp>

namespace lux::process::render_upload
{
    using namespace lux::render;
    constexpr error::ErrorDescriptor AssetLoadError{
        "lux.rendering.asset_load",
        "Asset loading stage {0}, storage error {1}, decode error {2}",
        error::ERecovery::NEEDS_INPUT,
        {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}
    };
    constexpr auto kAssetLoad = error::errorId(AssetLoadError.name);

    RenderError assetLoadError(const asset_loading::AssetLoadFailure& failure) noexcept
    {
        return {
            kAssetLoad,
            {static_cast<std::uint64_t>(failure.code),
             static_cast<std::uint64_t>(failure.storage_error),
             static_cast<std::uint64_t>(failure.decode.code)}
        };
    }

    struct PackedVertex final
    {
        float position[3];
        float uv[2];
        float normal[3];
        float tangent[4];
    };

    PackedMesh packMesh(const rdesc::Mesh& mesh) noexcept
    {
        PackedMesh packed;
        auto& description = packed.description;
        description.vertex_stride = sizeof(PackedVertex);
        description.vertex_count = static_cast<std::uint32_t>(mesh.vertices.size());
        description.index_offset = mesh.vertices.size() * sizeof(PackedVertex);
        description.index_count = static_cast<std::uint32_t>(mesh.indices.size());
        description.attributes = {
            {0, offsetof(PackedVertex, position), rdesc::ETextureFormat::RGB32_SFLOAT},
            {1, offsetof(PackedVertex, uv), rdesc::ETextureFormat::RG32_SFLOAT},
            {2, offsetof(PackedVertex, normal), rdesc::ETextureFormat::RGB32_SFLOAT},
            {3, offsetof(PackedVertex, tangent), rdesc::ETextureFormat::RGBA32_SFLOAT}
        };
        description.submeshes = {{0, description.index_count, 0}};
        if (mesh.bounds)
        {
            const auto center = mesh.bounds->center();
            const auto extent = mesh.bounds->halfExtents();
            description.bounds = {{center.x(), center.y(), center.z(), 1}, {extent.x(), extent.y(), extent.z(), 0}};
        }
        packed.bytes.resize(description.index_offset + mesh.indices.size() * sizeof(std::uint32_t));
        auto* vertices = reinterpret_cast<PackedVertex*>(packed.bytes.data());
        for (std::size_t i = 0; i < mesh.vertices.size(); ++i)
        {
            const auto& input = mesh.vertices[i];
            auto& output = vertices[i];
            std::memcpy(output.position, input.position.data(), sizeof(output.position));
            std::memcpy(output.uv, input.uv.data(), sizeof(output.uv));
            std::memcpy(output.normal, input.normal.data(), sizeof(output.normal));
            std::memcpy(output.tangent, input.tangent.data(), 3 * sizeof(float));
            output.tangent[3] = input.normal.cross(input.tangent).dot(input.bitangent) < 0 ? -1.f : 1.f;
        }
        std::memcpy(
            packed.bytes.data() + description.index_offset,
            mesh.indices.data(),
            mesh.indices.size() * sizeof(std::uint32_t)
        );
        return packed;
    }

    RenderResult<void> registerAssetErrors() noexcept
    {
        return error::ErrorRegistry::instance().registerTypes({&AssetLoadError, 1});
    }
} // namespace lux::process::render_upload
