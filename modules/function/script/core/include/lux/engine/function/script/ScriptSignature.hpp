#pragma once
/**
 * @file ScriptSignature.hpp
 * @brief C++-friendly snapshot of a script function signature.
 */

#include <cstdint>
#include <string>
#include <vector>

#include <lux/engine/function/script/abi/lux_script_abi.h>

namespace lux::script
{
    struct TypeDesc
    {
        std::string   name;
        std::uint64_t type_id = 0;
        std::uint32_t size    = 0;
        std::uint32_t align   = 0;
        std::uint8_t  kind    = LUX_SCRIPT_VK_VOID;

        static TypeDesc from(const lux_script_type_desc& d)
        {
            return TypeDesc{
                d.name ? std::string(d.name) : std::string{},
                d.type_id,
                d.size,
                d.align,
                d.kind
            };
        }
    };

    struct FunctionSignature
    {
        std::string             name;
        std::uint64_t           symbol_id = 0;
        std::vector<TypeDesc>   args;
        std::vector<TypeDesc>   returns;

        static FunctionSignature from(const lux_script_function_desc& d)
        {
            FunctionSignature out;
            out.name = d.name ? d.name : "";
            out.symbol_id = d.symbol_id;
            out.args.reserve(d.arg_count);
            for (std::uint32_t i = 0; i < d.arg_count; ++i)
                out.args.push_back(TypeDesc::from(d.args[i]));
            out.returns.reserve(d.return_count);
            for (std::uint32_t i = 0; i < d.return_count; ++i)
                out.returns.push_back(TypeDesc::from(d.returns[i]));
            return out;
        }
    };
}
