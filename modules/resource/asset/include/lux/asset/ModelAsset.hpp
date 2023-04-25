#pragma once
#include <lux/system/visibility_control.h>
#include <Eigen/Eigen>
#include <vector>
#include "LuxAsset.hpp"
#include "ImageAsset.hpp"

namespace lux::asset
{
    static constexpr uint8_t MAX_BONE_INFLUENCE = 4;

    struct Bone
    {
        int     bone_ids[MAX_BONE_INFLUENCE];
        float   weights[MAX_BONE_INFLUENCE];
    };

    struct Material
    {

    };

    struct Vertex
    {
        Eigen::Vector3f     position;
        Eigen::Vector3f     normal;
        Eigen::Vector3f     tangent;
        Eigen::Vector2f     texture_coordinates;

        // bitangent
        Eigen::Vector3f     bitangent;
        // bone influence
        Bone                bone;
    };

    enum class TextureType
    {
        AMBIENT,
        DIFFUSE,
        SPECULAR,
        HEIGHT,
        UNKNOWN
    };

    class TextureAsset : public ImageAsset
    {
        friend class ModelLoader;
    public:
        LUX_EXPORT explicit TextureAsset(FilePath path, bool flip_vertically = true);

        [[nodiscard]] TextureType type() const
        {
            return _type;
        }

        [[nodiscard]] float shininess() const
        {
            return _shininess;
        }

    private:
        TextureType _type;
        float       _shininess;
    };

    struct Mesh
    {
        std::vector<Vertex>     vertices;
        std::vector<uint32_t>   indices;
        std::vector<size_t>     texture_indices;
    };

    class ModelAsset : public LuxExternalAsset
    {
        friend class ModelLoader;
    public:
        LUX_EXPORT explicit ModelAsset(FilePath path);

        LUX_EXPORT LoadAssetResult load() override;

        LUX_EXPORT bool unload() override;

        [[nodiscard]] LUX_EXPORT bool isLoaded() const override;

        using TextureList   = std::vector<TextureAsset>;
        using MeshList      = std::vector<Mesh>;
        
        LUX_EXPORT TextureList& textureList();

        [[nodiscard]] LUX_EXPORT const TextureList& textureList() const;

        [[nodiscard]] LUX_EXPORT MeshList& meshList();

        [[nodiscard]] LUX_EXPORT const MeshList& meshList() const;

    private:
        TextureList     textures;
        MeshList        meshs;
    };
}
