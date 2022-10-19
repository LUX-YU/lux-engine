#include "lux-engine/core/math/EigenTools.hpp"
#include "graphic_api_wrapper/opengl3/ShaderProgram.hpp"

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <string>
#include <vector>
#include <iostream>
#include <sstream>

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

const char* vertexShader = 
R"(
#version 330 core
layout (location = 0) in vec3 position;
layout (location = 1) in vec3 normal;
layout (location = 2) in vec2 texCoords;
out vec2 TexCoords;
uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
void main()
{
    gl_Position = projection * view * model * vec4(position, 1.0f);
    TexCoords = texCoords;
}
)";

const char* fragshader = 
R"(
#version 330 core
in vec2 TexCoords;
out vec4 color;
uniform sampler2D texture_diffuse1;
void main()
{    
    color = vec4(texture(texture_diffuse1, TexCoords));
}
)";

struct Vertex
{
    Eigen::Vector3f Position;
    Eigen::Vector3f Normal;
    Eigen::Vector2f TexCoords;
};

struct Texture
{
    GLuint id;
    std::string type;
};

class Mesh
{
public:
    std::vector<Vertex>  vertices;
    std::vector<GLuint>  indices;
    std::vector<Texture> textures;

    Mesh(std::vector<Vertex> vertices, std::vector<GLuint> indices, std::vector<Texture> texture)
    {
        this->vertices = vertices;
        this->indices = indices;
        this->textures = textures;
        this->setupMesh();
    }
    
    void Draw(lux::engine::function::ShaderProgram shader_program)
    {
        GLuint diffuseNr = 1;
        GLuint specularNr = 1;
        for(GLuint i = 0; i < this->textures.size(); i++)
        {
            glActiveTexture(GL_TEXTURE0 + i); // 在绑定纹理前需要激活适当的纹理单元
            // 检索纹理序列号 (N in diffuse_textureN)
            std::stringstream ss;
            std::string number;
            std::string name = this->textures[i].type;
            if(name == "texture_diffuse")
                ss << diffuseNr++; // 将GLuin输入到string stream
            else if(name == "texture_specular")
                ss << specularNr++; // 将GLuin输入到string stream
            number = ss.str(); 

            shader_program.uniformSetValueUnsafe("material." + name + number, i);

            glBindTexture(GL_TEXTURE_2D, this->textures[i].id);
        }
        glActiveTexture(GL_TEXTURE0);

        // 绘制Mesh
        glBindVertexArray(this->VAO);
        glDrawElements(GL_TRIANGLES, this->indices.size(), GL_UNSIGNED_INT, 0);
        glBindVertexArray(0);
    }

private:
    GLuint VAO, VBO, EBO;
    void setupMesh()
    {
        glGenVertexArrays(1, &this->VAO);
        glGenBuffers(1, &this->VBO);
        glGenBuffers(1, &this->EBO);

        glBindVertexArray(this->VAO);
        glBindBuffer(GL_ARRAY_BUFFER, this->VBO);

        glBufferData(GL_ARRAY_BUFFER, this->vertices.size() * sizeof(Vertex), 
                     &this->vertices[0], GL_STATIC_DRAW);

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, this->EBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, this->indices.size() * sizeof(GLuint), 
                     &this->indices[0], GL_STATIC_DRAW);

        // 设置顶点坐标指针
        glEnableVertexAttribArray(0); 
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), 
                             (GLvoid*)0);
        // 设置法线指针
        glEnableVertexAttribArray(1); 
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), 
                             (GLvoid*)offsetof(Vertex, Normal));
        // 设置顶点的纹理坐标
        glEnableVertexAttribArray(2); 
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), 
                             (GLvoid*)offsetof(Vertex, TexCoords));

        glBindVertexArray(0);
    }
};

class Model 
{
    public:
        /*  成员函数   */
        Model(GLchar* path)
        {
            this->loadModel(path);
        }

        void Draw(lux::engine::function::ShaderProgram shader_program)
        {
            for(GLuint i = 0; i < this->meshes.size(); i++)
                this->meshes[i].Draw(shader_program);
        }
    private:
        /*  模型数据  */
        std::vector<Mesh> meshes;
        std::string directory;

        /*  私有成员函数   */
        void loadModel(std::string path)
        {
            Assimp::Importer import;
            const aiScene* scene = import.ReadFile(path, aiProcess_Triangulate | aiProcess_FlipUVs); 

            if(!scene || scene->mFlags == AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode) 
            {
                std::cout << "ERROR::ASSIMP::" << import.GetErrorString() << std::endl;
                return;
            }
            this->directory = path.substr(0, path.find_last_of('/'));

            this->processNode(scene->mRootNode, scene);
        }

        void processNode(aiNode* node, const aiScene* scene);
        Mesh processMesh(aiMesh* mesh, const aiScene* scene);
        std::vector<Texture> loadMaterialTextures(aiMaterial* mat, aiTextureType type, std::string typeName);
};


static int __main(int argc, char* argv[])
{
    return 0;
}

#include <lux-engine/platform/cxx/SubProgram.hpp>
RegistFunctionSubProgram(__main, "load_model")