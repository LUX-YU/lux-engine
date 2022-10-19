#pragma once
#include <lux-engine/platform/cxx/visibility_control.h>
#include <string>
#include <string_view>
#include <memory>

#include "Mesh.hpp"

namespace lux::engine::resource
{
    class Model;

    class ModelLoader
    {
    public:
        Model loadFrom(std::string path);

    private:
        class Impl;
        std::unique_ptr<Impl> _impl;
    };

    class Model
    {
        friend class ModelLoader;
    public:
        LUX_EXPORT std::string_view directory();

    protected:
        Model() = default;

    private:
        class ModelImpl;
        std::unique_ptr<ModelImpl> _impl;
    };
}
