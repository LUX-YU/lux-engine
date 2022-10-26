#pragma once
#include <string>
#include <vector>
#include "lux-engine/resource/model/Mesh.hpp"
#include "lux-engine/resource/model/Model.hpp"

namespace lux::engine::resource
{
    struct ModelImpl
    {
        std::string         path;
        std::string         directory;
        std::vector<Mesh>   meshs;
    };
}
