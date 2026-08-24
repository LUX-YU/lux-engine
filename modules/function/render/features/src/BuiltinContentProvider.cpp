#include <lux/engine/function/render/features/BuiltinContentProvider.hpp>

#include <array>

namespace lux::render
{
    namespace
    {
        struct BuiltinEntry final
        {
            EBuiltinShader shader{};
            std::span<const std::byte> spirv;
            std::span<const std::byte> metadata;
        };

        // The registry is intentionally source-owned and empty in the L1
        // build. A future Content/Toolchain provider can generate this table
        // without making Runtime link a host tool or include generated blobs.
        constexpr std::array<BuiltinEntry, 0> kBuiltinRegistry{};
    } // namespace

    lux::cxx::expected<BuiltinShaderContent, EBuiltinContentError>
    builtinShaderContent(EBuiltinShader shader) noexcept
    {
        for (const BuiltinEntry& entry : kBuiltinRegistry)
        {
            if (entry.shader == shader)
                return BuiltinShaderContent{entry.spirv, entry.metadata};
        }
        return lux::cxx::unexpected(
            EBuiltinContentError::BUILTIN_CONTENT_UNAVAILABLE
        );
    }
} // namespace lux::render
