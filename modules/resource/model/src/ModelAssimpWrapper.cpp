#include <lux-engine/resource/model/Model.hpp>
#include <lux-engine/platform/system/filesystem.hpp>

#include <vector>
#include <Eigen/Eigen>

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

namespace cxxstdfs = lux::engine::platform::cxxstdfs;

namespace lux::engine::resource
{
    class Model::ModelImpl
    {
    public:
        std::string         directory;
        std::vector<Mesh>   meshs;
    };

    std::string_view Model::directory()
    {
        return _impl->directory;
    }

    class ModelLoader::Impl
    {
    public:
        Model loadFrom(std::string path)
        {
            Model ret_model;

            const aiScene* scene = import.ReadFile(path, aiProcess_Triangulate | aiProcess_FlipUVs); 
            if(!scene || scene->mFlags == AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
            {
                return;
            }
            auto model_impl = processNode(scene->mRootNode, scene);
            model_impl->directory = cxxstdfs::path(path).parent_path().string();

            ret_model._impl = std::move(model_impl);
            return ret_model;
        }

        std::unique_ptr<Model::ModelImpl>
        processNode(aiNode* node, const aiScene* scene)
        {
            auto model_impl = std::make_unique<Model::ModelImpl>();
            for(unsigned int i = 0; i < node->mNumMeshes; i++)
            {
                aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
                
            }

            return model_impl;
        }

        Mesh processMesh(aiMesh* mesh, const aiScene* scene)
        {
            Mesh mesh;
        }

    private:
        Assimp::Importer import;
    };

    Model ModelLoader::loadFrom(std::string path)
    {
        return loadFrom(std::move(path));
    }
}