#pragma once

#include <cstdint>

namespace lux::engine::platform
{
// Same-SDK C++ ABI. Every pointer refers to storage owned by the loaded library.
// Entry points only describe code; they must not register or construct instances.
struct LibraryExportIdentity final
{
    std::uint32_t structure_size;
    std::uint32_t interface_version;
    const char *module_id;
    std::uint32_t module_version;
    const char *sdk_abi;
    const char *build_id;
    const char *declaration_digest;
};

using GetLibraryExportIdentity = const LibraryExportIdentity *() noexcept;
inline constexpr const char *kLibraryIdentitySymbol = "lux_plugin_identity_v1";
} // namespace lux::engine::platform
