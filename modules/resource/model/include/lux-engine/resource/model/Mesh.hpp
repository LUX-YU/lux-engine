#pragma once
#include <lux-engine/platform/cxx/visibility_control.h>
#include <vector>

#include "Vertex.hpp"

namespace lux::engine::resource
{
    enum class TextureType
    {
        DIFFUSE,
        SPECULAR
    };

    struct Texture
    {
        uint32_t    id;
        TextureType type;
    };

    class Mesh
    {
        std::vector<Vertex>     vertics;
        std::vector<uint32_t>   indices;
        std::vector<Texture>    textures;
    };
}
