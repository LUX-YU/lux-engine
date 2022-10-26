#pragma once
#include <lux-engine/platform/system/visibility_control.h>
#include <vector>
#include "Vertex.hpp"

namespace lux::engine::resource
{
    enum class TextureType
    {
        AMBIENT,
        DIFFUSE,
        SPECULAR
    };

    struct Texture
    {
        uint32_t    id;
        std::string path;
        TextureType type;
    };

    class Mesh
    {
    public:
        std::vector<Vertex>     vertices;
        std::vector<uint32_t>   indices;
        std::vector<Texture>    textures;
    };
}
