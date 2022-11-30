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
        HEIGHT,
        UNKNOWN
    };

    class Texture : public Image
    {
    public:
        Texture() = default;

        LUX_EXPORT Texture(std::string path, TextureType type, float shininess, bool flip_vertically = true);

        std::string path;
        TextureType type;
        float       shininess;
    };

    class Mesh
    {
    public:
        std::vector<Vertex>     vertices;
        std::vector<uint32_t>   indices;
        std::vector<size_t>     texture_indices;
    };
}
