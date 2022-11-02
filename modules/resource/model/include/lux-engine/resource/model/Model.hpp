#pragma once
#include <lux-engine/platform/system/visibility_control.h>
#include <lux-engine/platform/system/filesystem.hpp>
#include <lux-engine/resource/asset/LuxAsset.hpp>
#include <string>
#include <memory>

#include "Mesh.hpp"

namespace lux::engine::resource
{
    class Model;
    struct ModelImpl;

    class ModelLoader
    {
    public:
        LUX_EXPORT virtual std::unique_ptr<Model> loadFrom(const std::string& path);

    private:
        class Impl;
        std::unique_ptr<Impl> _impl;
    };

    class Model : public LuxAsset
    {
    public:
        std::string         path;
        std::string         directory;
        std::vector<Mesh>   meshs;
    };
}
