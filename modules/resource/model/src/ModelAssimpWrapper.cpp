#include <lux-engine/resource/model/Model.hpp>
#include <lux-engine/resource/model/ModelImpl.hpp>
#include <lux-engine/platform/system/filesystem.hpp>

#include <vector>
#include <Eigen/Eigen>

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

using LuxFSPath = ::lux::engine::platform::cxxstdfs::path;

namespace lux::engine::resource
{
    class ModelLoader::Impl
    {
    public:
        void loadFrom(const std::string& path)
        {
            ModelImpl ret_model_impl;

            const aiScene* scene = import.ReadFile(path, aiProcess_Triangulate | aiProcess_FlipUVs); 
            if(!scene || scene->mFlags == AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
            {
                return;
            }
            processNode(ret_model_impl, scene->mRootNode, scene);
            ret_model_impl.directory = LuxFSPath(path).parent_path().string();
            ret_model_impl.path      = LuxFSPath(path).string();
        }

        void processNode(ModelImpl& model, aiNode* node, const aiScene* scene)
        {
            for(size_t i = 0; i < node->mNumMeshes; i++)
            {
                aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
                model.meshs.push_back(this->processMesh(mesh, scene));
            }

            for(size_t i = 0; i < node->mNumChildren; i++)
            {
                this->processNode(model, node->mChildren[i], scene);
            }
        }

        Mesh processMesh(aiMesh* mesh, const aiScene* scene)
        {
            Mesh ret_mesh;
            // load vertices
            for(size_t i = 0; i < mesh->mNumVertices; i++)
            {
                Vertex vertex;
                vertex.position.x() = mesh->mVertices[i].x;
                vertex.position.y() = mesh->mVertices[i].y;
                vertex.position.z() = mesh->mVertices[i].z;
                // normal
                vertex.normal.x()   = mesh->mNormals[i].x;
                vertex.normal.y()   = mesh->mNormals[i].y;
                vertex.normal.z()   = mesh->mNormals[i].z;
                // texture
                vertex.texture_coordinates = 
                mesh->mTextureCoords[0] ? Eigen::Vector2f{
                    mesh->mTextureCoords[0][i].x,
                    mesh->mTextureCoords[0][i].y
                } : Eigen::Vector2f{0,0};

                ret_mesh.vertices.push_back(vertex);
            }
            // load each of mesh's face
            for(size_t i = 0; i < mesh->mNumFaces; i++)
            {
                aiFace& face = mesh->mFaces[i];
                for(size_t j = 0; j < face.mNumIndices; j++)
                {
                    ret_mesh.indices.push_back(face.mIndices[j]);
                }
            }
            // process meterials
            if(mesh->mMaterialIndex >= 0)
            {
                aiMaterial* material = scene->mMaterials[mesh->mMaterialIndex];
                // diffuse map
                loadMaterialTextures(ret_mesh.textures, material, aiTextureType_DIFFUSE);
                loadMaterialTextures(ret_mesh.textures, material, aiTextureType_SPECULAR);
            }
        }

        static TextureType assimpTextureEnumConverter(aiTextureType type)
        {
            switch (type)
            {
            case aiTextureType::aiTextureType_AMBIENT :
                return TextureType::AMBIENT;
            case aiTextureType::aiTextureType_DIFFUSE :
                return TextureType::DIFFUSE;
            case aiTextureType::aiTextureType_SPECULAR :
                return TextureType::SPECULAR;
            default:
                break;
            }
        }

        void loadMaterialTextures(std::vector<Texture>& current_textures, aiMaterial* mat, aiTextureType type)
        {
            std::vector<Texture> textures;
            for(size_t i = 0; i < mat->GetTextureCount(type); i++)
            {
                bool skip = false;
                aiString str;
                mat->GetTexture(type, i, &str);
                for(size_t j = 0; j < current_textures.size(); j++)
                {
                    if(current_textures[j].path == str.C_Str())
                    {
                        textures.push_back(current_textures[j]);
                        skip = true;
                        break;
                    }
                }
                if(!skip)
                {
                    Texture texture;
                    texture.type = assimpTextureEnumConverter(type);
                    texture.path = str.C_Str();
                    current_textures.emplace_back(std::move(texture));
                }
            }
            current_textures.insert(current_textures.end(), textures.begin(), textures.end());
        }

    private:
        Assimp::Importer import;
    };

    void ModelLoader::loadFrom(const std::string& path)
    {
    
    }
}