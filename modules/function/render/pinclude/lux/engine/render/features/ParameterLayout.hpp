#pragma once

#include <lux/engine/description/ShaderBlock.hpp>

namespace lux::render::detail
{
    [[nodiscard]] std::size_t parameterLayoutBytes(const rdesc::ShaderBlock&) noexcept;
} // namespace lux::render::detail
