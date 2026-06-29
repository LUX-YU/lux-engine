#pragma once
/**
 * @file ResourceUploadCmd.hpp
 * @brief Command types consumed by ResourceSyncHub resource channels.
 *
 * These commands are pushed from game-thread resource sync producers
 * into Hub-managed SPSC queues, then popped and processed on the render
 * thread. Each command carries all data needed to perform the GPU
 * resource operation (mesh upload, texture upload, material update, etc.).
 *
 * Design notes
 * ------------
 *   - Data ownership: vertex/pixel data is deep-copied into the command
 *     at push time (game-thread-owned).  The upload thread consumes it
 *     without any aliasing.
 *   - No Vulkan types in the public API.
 *   - Material descriptions use MaterialDesc (render-layer POD), not
 *     asset-layer material types.
 */

#include <lux/engine/render/core/RenderResourceHandle.hpp>
#include <lux/engine/render/core/VertexLayoutTypes.hpp>
#include <lux/engine/render/core/LightDescriptor.hpp>
#include <lux/engine/render/core/MaterialFamily.hpp>
#include <lux/engine/render/core/RenderTypes.hpp>
#include <lux/engine/math/AABB.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace lux::render
{

    // =========================================================================
    //  Texture format enumeration — now defined in RenderTypes.hpp
    // =========================================================================

    // =========================================================================
    //  MaterialDesc — render-layer material descriptor (no asset types)
    // =========================================================================

    /**
     * @brief Lightweight material description for the render layer.
     *
     * Contains only raw data + RTextureHandle references.  No asset_id_t,
     * no asset::Material, no Vulkan types.  The upload thread converts
     * this to the GPU-layout struct and writes into the material SSBO.
     */
    struct MaterialDesc
    {
        ELightingTechnique      family{ ELightingTechnique::Unlit };
        EShadingModel           shading_model{ EShadingModel::INVALID };

        /// Generic parameter block — holds the shading model's parameters
        /// as a flat byte blob (size ≤ 256).  The upload thread reinterprets
        /// it based on (family, shading_model).
        std::vector<uint8_t>    params_data;

        /// Texture handles (invalid handle ⇒ no texture bound for that slot).
        RTextureHandle          base_color{};
        RTextureHandle          normal_map{};
        RTextureHandle          metallic_roughness{};
        RTextureHandle          occlusion{};
        RTextureHandle          emissive{};
        // Additional slots can be added as needed.

        uint32_t                flags{ 0 };           ///< MATF_DOUBLE_SIDED, etc.
    };

    // =========================================================================
    //  Upload commands (game → upload thread)
    // =========================================================================

    struct UploadMeshCmd
    {
        RMeshHandle                handle;
        VertexLayoutId             layout_id{ kInvalidVertexLayoutId };
        uint32_t                   vertex_stride{ 0 };
        std::vector<std::byte>     vertex_data;     ///< Owning copy
        std::vector<uint32_t>      index_data;      ///< Owning copy
        std::optional<lux::math::AABB> bounds;
    };

    struct DestroyMeshCmd
    {
        RMeshHandle handle;
    };

    // -------------------------------------------------------------------------
    struct UploadTexture2DCmd
    {
        RTextureHandle              handle;
        std::vector<uint8_t>        pixels;         ///< Owning copy
        int32_t                     width{ 0 };
        int32_t                     height{ 0 };
        int32_t                     channels{ 4 };
        EPixelFormat                format{ EPixelFormat::RGBA8_SRGB };
        bool                        generate_mips{ true };
    };

    struct UploadCubeTextureCmd
    {
        RTextureHandle              handle;
        std::array<std::vector<uint8_t>, 6> face_data;  ///< 6 face pixel data
        int32_t                     face_size{ 0 };
        int32_t                     channels{ 4 };
        EPixelFormat                format{ EPixelFormat::RGBA8_SRGB };
    };

    struct DestroyTextureCmd
    {
        RTextureHandle handle;
    };

    // -------------------------------------------------------------------------

    struct UploadMaterialCmd
    {
        RMaterialHandle  handle;
        MaterialDesc     desc;
    };

    struct ModifyMaterialCmd
    {
        RMaterialHandle  handle;
        MaterialDesc     desc;
    };

    struct DestroyMaterialCmd
    {
        RMaterialHandle handle;
    };

    // -------------------------------------------------------------------------

    struct CreateLightCmd
    {
        RLightHandle     handle;
        LightDescriptor  descriptor;
    };

    struct UpdateLightCmd
    {
        RLightHandle     handle;
        LightDescriptor  descriptor;
    };

    struct DestroyLightCmd
    {
        RLightHandle handle;
    };

    // =========================================================================
    //  Per-resource-type command variants (used by ResourceSyncHub queues)
    // =========================================================================

    using MeshCmdVariant     = std::variant<UploadMeshCmd, DestroyMeshCmd>;
    using TextureCmdVariant  = std::variant<UploadTexture2DCmd, UploadCubeTextureCmd, DestroyTextureCmd>;
    using MaterialCmdVariant = std::variant<UploadMaterialCmd, ModifyMaterialCmd, DestroyMaterialCmd>;
    using LightCmdVariant    = std::variant<CreateLightCmd, UpdateLightCmd, DestroyLightCmd>;

} // namespace lux::render
