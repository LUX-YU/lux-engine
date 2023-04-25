#include <lux/asset/ModelAsset.hpp>
#include <lux/system/file_system.hpp>

#include <vector>
#include <Eigen/Eigen>

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <type_traits>

namespace lux::asset
{
    class ModelLoader
    {
    public:
        LoadAssetResult loadFrom(const FilePath& path, ModelAsset& model_asset)
        {
            const aiScene* scene = import.ReadFile(path.string().c_str(), aiProcess_Triangulate | aiProcess_FlipUVs);
            if (!scene || scene->mFlags == AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
            {
                return LoadAssetResult::UNKNOWN_ERROR;
            }
            processNode(model_asset, scene->mRootNode, scene);
            return LoadAssetResult::SUCCESS;
        }

        void processNode(ModelAsset& model, aiNode* node, const aiScene* scene)
        {
            for (size_t i = 0; i < node->mNumMeshes; i++)
            {
                aiMesh* ai_mesh = scene->mMeshes[node->mMeshes[i]];
                Mesh mesh; processMesh(model, mesh, ai_mesh, scene);
                model.meshs.emplace_back(std::move(mesh));
            }

            for (size_t i = 0; i < node->mNumChildren; i++)
            {
                this->processNode(model, node->mChildren[i], scene);
            }
        }

        void processMesh(ModelAsset& model, Mesh& ret_mesh, aiMesh* mesh, const aiScene* scene)
        {
            // load vertices
            for (size_t i = 0; i < mesh->mNumVertices; i++)
            {
                Vertex vertex;
                vertex.position.x() = mesh->mVertices[i].x;
                vertex.position.y() = mesh->mVertices[i].y;
                vertex.position.z() = mesh->mVertices[i].z;
                // normal
                if (mesh->mNormals)
                {
                    vertex.normal.x() = mesh->mNormals[i].x;
                    vertex.normal.y() = mesh->mNormals[i].y;
                    vertex.normal.z() = mesh->mNormals[i].z;
                }

                // texture
                if (mesh->mTextureCoords[0])
                {
                    // texture coordinate
                    vertex.texture_coordinates.x() = mesh->mTextureCoords[0][i].x;
                    vertex.texture_coordinates.y() = mesh->mTextureCoords[0][i].y;

                    // tangent
                    if (mesh->mTangents)
                    {
                        vertex.tangent.x() = mesh->mTangents[i].x;
                        vertex.tangent.y() = mesh->mTangents[i].y;
                        vertex.tangent.z() = mesh->mTangents[i].z;
                    }

                    // bitangent
                    if (mesh->mBitangents)
                    {
                        vertex.bitangent.x() = mesh->mBitangents[i].x;
                        vertex.bitangent.y() = mesh->mBitangents[i].y;
                        vertex.bitangent.z() = mesh->mBitangents[i].z;
                    }
                }

                ret_mesh.vertices.push_back(vertex);
            }
            // load each of mesh's face
            for (size_t i = 0; i < mesh->mNumFaces; i++)
            {
                aiFace& face = mesh->mFaces[i];
                for (size_t j = 0; j < face.mNumIndices; j++)
                {
                    ret_mesh.indices.push_back(face.mIndices[j]);
                }
            }
            // process meterials
            if (mesh->mMaterialIndex >= 0)
            {
                aiMaterial* material = scene->mMaterials[mesh->mMaterialIndex];
                // diffuse map
                loadMaterialTextures(model, ret_mesh, material, aiTextureType_AMBIENT);
                loadMaterialTextures(model, ret_mesh, material, aiTextureType_DIFFUSE);
                loadMaterialTextures(model, ret_mesh, material, aiTextureType_SPECULAR);
                loadMaterialTextures(model, ret_mesh, material, aiTextureType_HEIGHT);
            }
        }

        static TextureType assimpTextureEnumConverter(aiTextureType type)
        {
            switch (type)
            {
            case aiTextureType::aiTextureType_AMBIENT:
                return TextureType::AMBIENT;
            case aiTextureType::aiTextureType_DIFFUSE:
                return TextureType::DIFFUSE;
            case aiTextureType::aiTextureType_SPECULAR:
                return TextureType::SPECULAR;
            case aiTextureType::aiTextureType_HEIGHT:
                return TextureType::HEIGHT;
            }
            return TextureType::UNKNOWN;
        }

        void loadMaterialTextures(ModelAsset& model, Mesh& mesh, aiMaterial* mat, aiTextureType type)
        {
            using Integer = decltype(std::declval<aiMaterial>().GetTextureCount(type));
            Integer texture_count = mat->GetTextureCount(type);
            auto& current_textures = model.textures;
            for (Integer i = 0; i < texture_count; i++)
            {
                bool skip = false;
                // get current material texture path
                aiString str; mat->GetTexture(type, i, &str);
                // get texture absolute path
                auto parent_directory = model.filePath().parent_path();
                auto texture_absolute_path = parent_directory / str.C_Str();
                for (size_t j = 0; j < model.textures.size(); j++)
                {
                    // already loaded
                    if (current_textures[j].filePath() == texture_absolute_path)
                    {
                        mesh.texture_indices.push_back(j);
                        skip = true;
                        break;
                    }
                }
                if (!skip)
                {
                    float shininess;
                    // get shininess
                    if (AI_SUCCESS != aiGetMaterialFloat(mat, AI_MATKEY_SHININESS, &shininess))
                    {
                        // if unsuccessful set a default
                        shininess = 16.f;
                    }
                    TextureAsset texture(texture_absolute_path.string());
                    texture._type      = assimpTextureEnumConverter(type);
                    texture._shininess = shininess;

                    current_textures.emplace_back(std::move(texture));
                    mesh.texture_indices.push_back(current_textures.size() - 1);
                }
            }
        }
    private:
        Assimp::Importer import;
    };

    TextureAsset::TextureAsset(FilePath path, bool flip_vertically)
    : ImageAsset(std::move(path), flip_vertically), _type(TextureType::UNKNOWN) , _shininess(16.0f)
    {
    }

    // static model loader
    static ModelLoader model_loader;

    ModelAsset::ModelAsset(FilePath path)
        : LuxExternalAsset(std::move(path)) {

    }

    LoadAssetResult ModelAsset::load()
    {
        LoadAssetResult rst{ LoadAssetResult::UNKNOWN_ERROR };
        if (model_loader.loadFrom(filePath(), *this) == LoadAssetResult::SUCCESS)
        {
            for (auto& texture : textures)
            {
                auto _texture_laod_rst = texture.load();
                if (_texture_laod_rst != LoadAssetResult::SUCCESS)
                {
                    rst = LoadAssetResult::RELATED_ASSET_LOAD_ERROR;
                }
            }
        }
        else
        {
            rst = LoadAssetResult::UNKNOWN_ERROR;
        }
        return rst;
    }

    bool ModelAsset::unload()
    {
        TextureList ttmp;
        textures.swap(ttmp);
        MeshList mtmp;
        meshs.swap(mtmp);
        return true;
    }

    bool ModelAsset::isLoaded() const
    {
        return !meshs.empty();
    }

    ModelAsset::TextureList& ModelAsset::textureList()
    {
        return textures;
    }

    const ModelAsset::TextureList& ModelAsset::textureList() const
    {
        return textures;
    }

    ModelAsset::MeshList& ModelAsset::meshList()
    {
        return meshs;
    }

    const ModelAsset::MeshList& ModelAsset::meshList() const
    {
        return meshs;
    }
}