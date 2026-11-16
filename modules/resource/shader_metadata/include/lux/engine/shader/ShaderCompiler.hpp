#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/description/ShaderProgramArtifact.hpp>
#include <lux/engine/shader/visibility.h>

namespace lux::shader
{
    enum class EShaderCompileError : std::uint8_t
    {
        INVALID_INPUT,
        COMPILER_FAILURE,
        UNSUPPORTED_LAYOUT
    };

    struct ShaderCompileFailure final
    {
        EShaderCompileError code;
        std::string diagnostic;
    };

    struct ShaderCompileRequest final
    {
        std::string_view source;
        std::string_view entry;
        rdesc::EShaderType stage;
        rdesc::EShaderTarget target;
        std::string_view profile = "sm_6_6";
        std::string_view source_path = "lux_shader.slang";
        std::span<const std::string_view> include_directories;
    };

    using ShaderCompileResult = cxx::expected<rdesc::ShaderProgramArtifact, ShaderCompileFailure>;

    // Synchronous cold compilation. The result owns its binary and reflection from that same target.
    [[nodiscard]] LUX_SHADER_COMPILER_PUBLIC ShaderCompileResult compile(const ShaderCompileRequest& request) noexcept;
    [[nodiscard]] LUX_SHADER_COMPILER_PUBLIC std::string_view compilerVersion() noexcept;
} // namespace lux::shader
