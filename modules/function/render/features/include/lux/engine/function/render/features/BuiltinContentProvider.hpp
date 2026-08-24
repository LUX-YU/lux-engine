#pragma once

#include <lux/engine/function/render/client/resources/EBuiltinShader.hpp>
#include <lux/engine/function/render/features/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace lux::render
{
    enum class EBuiltinContentError : std::uint8_t
    {
        BUILTIN_CONTENT_UNAVAILABLE,
    };

    struct BuiltinShaderContent final
    {
        std::span<const std::byte> spirv;
        std::span<const std::byte> metadata;
    };

    [[nodiscard]] LUX_ENGINE_FUNCTION_RENDER_FEATURES_PUBLIC
    lux::cxx::expected<BuiltinShaderContent, EBuiltinContentError>
    builtinShaderContent(EBuiltinShader shader) noexcept;
} // namespace lux::render
