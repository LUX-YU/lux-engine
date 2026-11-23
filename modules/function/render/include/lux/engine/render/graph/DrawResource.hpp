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

    namespace extension_detail
    {
        struct DrawSourceAddress final
        {
            std::uint32_t pass, source;
        };
    }

    template <class Storage> class DrawResourceSlot final
    {
        friend class CpuCallFrame;
        friend class RasterRecordWriter;
        friend class RasterOperands;
        explicit constexpr DrawResourceSlot(extension_detail::DrawSourceAddress address) noexcept : address_(address) {}
        extension_detail::DrawSourceAddress address_;
    };

    struct DrawResourceInput final
    {
        std::string binding;
        DrawResourceType source;
        std::uint32_t required_capabilities = 0;
        bool optional = false;
        bool indexed_geometry = false;
    };
}
