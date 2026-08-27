#pragma once
#include <string>
#include <vector>
#include <memory>
#include <cstdint>

namespace lux::rdesc
{
    struct ModelMeshInfo
    {
        uint32_t mesh_index{0};
        uint32_t material_index{0};
        std::string name;
    };

    struct ModelNode
    {
        std::vector<std::unique_ptr<ModelNode>> children;
        std::vector<ModelMeshInfo> mesh_infos;
        std::string name;
    };
}
