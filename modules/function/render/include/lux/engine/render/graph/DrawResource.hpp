#pragma once

#include <lux/cxx/compile_time/type_info.hpp>
#include <lux/engine/render/core/Resource.hpp>
#include <string>

namespace lux::render
{
    struct DrawResourceType final
    {
        ResourceName type;
        cxx::basic_type_info implementation;
    };

    template <class Storage> class DrawResourceSlot final
    {
        friend class FramePreparation;
        friend class InstanceLinkContext;
        constexpr DrawResourceSlot(std::uint32_t pass, std::uint32_t source) noexcept : pass_(pass), source_(source) {}
        std::uint32_t pass_, source_;
    };

    struct DrawResourceInput final
    {
        std::string binding;
        DrawResourceType source;
        std::uint32_t required_capabilities = 0;
        bool optional = false;
    };
}
