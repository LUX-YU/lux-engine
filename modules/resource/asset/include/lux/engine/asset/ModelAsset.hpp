#pragma once
#include <lux/engine/resource/visibility.h>
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

    enum class ETextureType
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
        LUX_RESOURCE_PUBLIC explicit TextureAsset(FilePath path, bool flip_vertically = true);

        [[nodiscard]] ETextureType type() const
        {
            return _type;
        }

        [[nodiscard]] float shininess() const
        {
            return _shininess;
        }

    private:
        ETextureType _type;
        float        _shininess;
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
        using TextureList = std::vector<TextureAsset>;
        using MeshList    = std::vector<Mesh>;

        LUX_RESOURCE_PUBLIC explicit
        ModelAsset(FilePath path);

        LUX_RESOURCE_PUBLIC LoadAssetResult    
        load() override;

        LUX_RESOURCE_PUBLIC bool unload() override;

        [[nodiscard]] LUX_RESOURCE_PUBLIC bool isLoaded() const override;

        LUX_RESOURCE_PUBLIC TextureList& textureList();

        [[nodiscard]] LUX_RESOURCE_PUBLIC const TextureList& textureList() const;

        [[nodiscard]] LUX_RESOURCE_PUBLIC MeshList& meshList();

        [[nodiscard]] LUX_RESOURCE_PUBLIC const MeshList& meshList() const;

    private:
        TextureList     textures;
        MeshList        meshs;
    };
}
