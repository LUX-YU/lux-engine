#pragma once

#include <cstddef>
#include <lux/engine/render/extensions/graph/ConstantBlock.hpp>
#include <span>
#include <vector>

namespace lux::render
{
    struct ParameterBinding final
    {
        std::vector<std::byte> values;
        // Explicitly zero-initialize any reflected tail beyond the supplied constant bytes.
        bool zero_fill_tail = false;
        ConstantBlockContract inline_constant;

        template <ShaderConstantBlock T> void setInlineConstant(const T& value) noexcept
        {
            const auto bytes = std::as_bytes(std::span{&value, 1});
            values.assign(bytes.begin(), bytes.end());
            zero_fill_tail = false;
            inline_constant = kConstantBlockContract<T>;
        }
    };
} // namespace lux::render
