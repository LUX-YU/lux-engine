#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/description/ShaderProgramArtifact.hpp>
#include <lux/engine/shader/visibility.h>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

    enum class EShaderParameterUse : std::uint8_t
    {
        UNKNOWN,
        UNUSED,
        USED
    };

    struct ShaderParameterUse final
    {
        std::string name;
        std::uint32_t space = 0;
        std::uint32_t binding = 0;
        EShaderParameterUse use = EShaderParameterUse::UNKNOWN;
    };

    // Compilation diagnostics and actual use are separate from the declared runtime layout.
    struct ShaderCompileReport final
    {
        std::string diagnostic;
        std::vector<ShaderParameterUse> parameters;
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
        ShaderCompileReport* report = nullptr;
        // Optional file dependencies of this request, including imported modules and their includes.
        std::vector<std::string>* dependencies = nullptr;
    };

    using ShaderCompileResult = cxx::expected<rdesc::ShaderProgramArtifact, ShaderCompileFailure>;
    using ShaderCompileBatchResult = cxx::expected<std::vector<rdesc::ShaderProgramArtifact>, ShaderCompileFailure>;

    // Serial synchronous owner; parallel work uses independent instances on the existing CPU scheduler.
    // A batch borrows immutable source files/dependencies until return. Target/options select a Session;
    // all Sessions retire at batch end, after copying results. A later batch reloads changed dependencies.
    class LUX_SHADER_COMPILER_PUBLIC ShaderCompiler final
    {
    public:
        using CreateResult = cxx::expected<std::unique_ptr<ShaderCompiler>, ShaderCompileFailure>;
        [[nodiscard]] static CreateResult create() noexcept;
        ~ShaderCompiler() noexcept;
        ShaderCompiler(const ShaderCompiler&) = delete;
        ShaderCompiler& operator=(const ShaderCompiler&) = delete;
        [[nodiscard]] ShaderCompileBatchResult compileBatch(std::span<const ShaderCompileRequest> requests) noexcept;
        [[nodiscard]] ShaderCompileResult compile(const ShaderCompileRequest& request) noexcept;

    private:
        struct Impl;
        explicit ShaderCompiler(std::unique_ptr<Impl>) noexcept;
        std::unique_ptr<Impl> impl_;
    };

    [[nodiscard]] LUX_SHADER_COMPILER_PUBLIC std::string_view compilerVersion() noexcept;
} // namespace lux::shader
