#pragma once
#include <glad/glad.h>
#include <lux-engine/resource/model/Model.hpp>
#include <lux-engine/function/graphic_api/opengl3/ShaderProgram.hpp>

namespace lux::engine::function
{
    namespace lux_ns_resource = ::lux::engine::resource;
    namespace lux_ns_function = ::lux::engine::function;

    struct GLTexture
    {
        GLuint texture_object_handle;
        float  shininess;
        lux_ns_resource::TextureType type;
    };

    class GLMesh
    {
        friend class GLModel;

    private:
        GLuint VAO;
        GLuint VBO;
        GLuint EBO;
        std::vector<GLTexture> texture_objs;
        size_t indics_size;

        GLMesh() = default;
    };

    class GLModel
    {
        using memory_mesh       = lux_ns_resource::Mesh;
        using gl_shader_program = lux_ns_function::ShaderProgram;

    private:
        std::vector<GLTexture> texture_objs;
        std::vector<GLMesh> mesh_objs;

    public:
        GLModel(lux_ns_resource::Model &model)
        {
            convert2glTexture(model);
            convert2glMesh(model);
        }

        void draw(gl_shader_program &shader_program)
        {
            using UnderlyingType = std::underlying_type_t<lux_ns_resource::TextureType>;
            static constexpr auto type_num = static_cast<UnderlyingType>(lux_ns_resource::TextureType::UNKNOWN);
            static const char *type2str[type_num]{
                "material.texture_ambient1",
                "material.texture_diffuse1",
                "material.texture_specular1"
            };

            for (size_t i = 0; i < mesh_objs.size(); i++)
            {
                auto &mesh = mesh_objs[i];
                // bind texture
                GLuint type_list[3]{0, 0, 0};
                for (GLenum j = 0; j < mesh.texture_objs.size(); j++)
                {
                    glActiveTexture(GL_TEXTURE0 + j);
                    
                    auto type_number = static_cast<UnderlyingType>(mesh.texture_objs[j].type);
                    auto name = type2str[type_number];
                    if (type_list[type_number]++)
                    {
                        // unsupport number of material light attribute
                        assert(type_list[type_number] > 1);
                    }
                    auto location = shader_program.uniformFindLocationUnsafe(name);
                    shader_program.uniformSetValue<int>(location, j);
                    shader_program.uniformSetValueUnsafe("material.shininess", mesh.texture_objs[j].shininess);
                    glBindTexture(GL_TEXTURE_2D, mesh.texture_objs[j].texture_object_handle);
                }
                // draw mesh
                glBindVertexArray(mesh.VAO);
                glDrawElements(GL_TRIANGLES, mesh.indics_size, GL_UNSIGNED_INT, 0);
                glBindVertexArray(0);
            }
        }

    private:

        void convert2glTexture(lux_ns_resource::Model &model)
        {
            auto &textures = model.textures;
            for (auto &texture : textures)
            {
                GLuint texture_id;
                auto width = texture.width();
                auto height = texture.height();
                glGenTextures(1, &texture_id);
                glBindTexture(GL_TEXTURE_2D, texture_id);
                glTexImage2D(
                    GL_TEXTURE_2D, 0, GL_RGB, width, height,
                    0, GL_RGB, GL_UNSIGNED_BYTE, texture.data());
                glGenerateMipmap(GL_TEXTURE_2D);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glBindTexture(GL_TEXTURE_2D, 0);
                texture_objs.push_back({texture_id, texture.shininess, texture.type});
            }
        }

        void convert2glMesh(lux_ns_resource::Model &model)
        {
            static constexpr auto VertexSize      = sizeof(lux_ns_resource::Vertex);
            static constexpr auto position_offset = offsetof(lux_ns_resource::Vertex, position);
            static constexpr auto normal_offset   = offsetof(lux_ns_resource::Vertex, normal);
            static constexpr auto texture_offset  = offsetof(lux_ns_resource::Vertex, texture_coordinates);

            for (auto &mesh : model.meshs)
            {
                GLMesh gl_mesh;
                gl_mesh.indics_size = mesh.indices.size();
                glGenVertexArrays(1, &gl_mesh.VAO);
                glGenBuffers(1, &gl_mesh.VBO);
                glGenBuffers(1, &gl_mesh.EBO);

                glBindVertexArray(gl_mesh.VAO);
                glBindBuffer(GL_ARRAY_BUFFER, gl_mesh.VBO);
                glBufferData(GL_ARRAY_BUFFER, mesh.vertices.size() * VertexSize, &mesh.vertices[0], GL_STATIC_DRAW);

                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl_mesh.EBO);
                glBufferData(GL_ELEMENT_ARRAY_BUFFER, mesh.indices.size() * sizeof(GLuint), &mesh.indices[0], GL_STATIC_DRAW);

                // set vertex attribute array(VAO)
                glEnableVertexAttribArray(0);
                glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, VertexSize, (GLvoid *)position_offset);
                // set normal
                glEnableVertexAttribArray(1);
                glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, VertexSize, (GLvoid *)normal_offset);
                // set texture
                glEnableVertexAttribArray(2);
                glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, VertexSize, (GLvoid *)texture_offset);

                for (auto &mesh_index : mesh.texture_indices)
                {
                    gl_mesh.texture_objs.push_back(this->texture_objs[mesh_index]);
                }

                mesh_objs.push_back(gl_mesh);
            }
            glBindVertexArray(0);
        }
    };
} // namespace lux::engine::function
