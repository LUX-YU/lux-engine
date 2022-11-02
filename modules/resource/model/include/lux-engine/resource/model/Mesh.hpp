#pragma once
#include <lux-engine/platform/system/visibility_control.h>
#include <lux-engine/resource/image/Image.hpp>
#include <vector>
#include "Vertex.hpp"

namespace lux::engine::resource
{
    enum class TextureType
    {
        AMBIENT,
        DIFFUSE,
        SPECULAR,
        UNKNOWN
    };

    class Texture : public Image
    {
    public:
        Texture(std::string path, bool flip_vertically = true)
            : Image(path, flip_vertically), path(std::move(path)){}

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
