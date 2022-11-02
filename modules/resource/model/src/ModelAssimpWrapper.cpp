#include <lux-engine/resource/model/Model.hpp>
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
        std::unique_ptr<Model> loadFrom(const std::string& path)
        {
            auto ret_model_impl = std::make_unique<Model>();

            const aiScene* scene = import.ReadFile(path, aiProcess_Triangulate | aiProcess_FlipUVs); 
            if(!scene || scene->mFlags == AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
            {
                return nullptr;
            }
            processNode(*ret_model_impl, scene->mRootNode, scene);
            ret_model_impl->directory = LuxFSPath(path).parent_path().string();
            ret_model_impl->path      = LuxFSPath(path).string();
            return ret_model_impl;
        }

        void processNode(Model& model, aiNode* node, const aiScene* scene)
        {
            for(size_t i = 0; i < node->mNumMeshes; i++)
            {
                aiMesh* ai_mesh = scene->mMeshes[node->mMeshes[i]];
                Mesh mesh; processMesh(mesh, ai_mesh, scene);
                model.meshs.emplace_back(std::move(mesh));
            }

            for(size_t i = 0; i < node->mNumChildren; i++)
            {
                this->processNode(model, node->mChildren[i], scene);
            }
        }

        void processMesh(Mesh& ret_mesh, aiMesh* mesh, const aiScene* scene)
        {
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
            }
            return TextureType::UNKNOWN;
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
                    Texture texture(str.C_Str());
                    texture.type = assimpTextureEnumConverter(type);
                    current_textures.emplace_back(std::move(texture));
                }
            }
            current_textures.insert(current_textures.end(), textures.begin(), textures.end());
        }

    private:
        Assimp::Importer import;
    };

    std::unique_ptr<Model> ModelLoader::loadFrom(const std::string& path)
    {
        return _impl->loadFrom(path);
    }
}