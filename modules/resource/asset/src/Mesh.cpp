#pragma once
#include <lux-engine/resource/model/Mesh.hpp>

namespace lux::engine::resource
{
    Texture::Texture(std::string _path, TextureType _type, float _shininess, bool flip_vertically)
            : Image(_path, flip_vertically), path(std::move(_path))
    {
        this->type = _type;
        this->shininess = _shininess;
    }
} // namespace lux::engine::resource
