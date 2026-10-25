#pragma once

#include <lux/engine/render/extensions/graph/GraphResource.hpp>
#include <string>
#include <string_view>

namespace lux::render::detail
{
    inline std::string quoteDumpText(std::string_view text) noexcept
    {
        constexpr char hex[] = "0123456789abcdef";
        std::string output{"\""};
        for (unsigned char c : text)
        {
            if (c == '"' || c == '\\')
            {
                output += '\\';
                output += static_cast<char>(c);
            }
            else if (c < 32)
            {
                output += "\\u00";
                output += hex[c >> 4];
                output += hex[c & 15];
            }
            else
            {
                output += static_cast<char>(c);
            }
        }
        output += '"';
        return output;
    }

    inline std::string dumpRange(const VGraphRange& range) noexcept
    {
        if (const auto* buffer = std::get_if<GraphBufferRange>(&range))
        {
            return "{\"offset\":" + std::to_string(buffer->offset) + ",\"size\":" + std::to_string(buffer->size) + "}";
        }
        if (const auto* image = std::get_if<GraphImageRange>(&range))
        {
            return "{\"aspects\":" + std::to_string(image->aspects) + ",\"mip\":" + std::to_string(image->first_mip) +
                   ",\"mips\":" + std::to_string(image->mip_count) +
                   ",\"layer\":" + std::to_string(image->first_layer) +
                   ",\"layers\":" + std::to_string(image->layer_count) + "}";
        }
        return "\"whole\"";
    }
} // namespace lux::render::detail
