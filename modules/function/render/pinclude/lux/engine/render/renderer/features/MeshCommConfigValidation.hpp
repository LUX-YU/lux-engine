#pragma once
// ============================================================================
//  MeshCommConfigValidation.hpp — shared create-fn preamble for the GPU-driven
//  mesh feature factories (DeferredGBuffer / ForwardMesh / MeshShadow).
//
//  Every mesh feature's create-fn validates its CommConfig payload identically:
//  size-check the blob, copy it, then reject an unsupported comm-config or
//  descriptor-layout version or any unknown extension flag. Only the Config type,
//  the version constants, the known-flag mask, and the log tag differ — so the
//  body lives here once, parameterised by them. The op-registration half is
//  already shared via FeatureOpRegistrar; this is the create-fn half.
// ============================================================================

#include <cstddef>
#include <cstdint>
#include <iostream>

namespace lux::render
{
    /**
     * @brief Validate a mesh feature's CommConfig payload and copy it out.
     *
     *        Checks, in order: a non-null blob of exactly `sizeof(Config)` bytes,
     *        a matching comm_config_version, a matching descriptor_layout_version,
     *        and no unknown extension_flags. On any failure it logs to std::cerr
     *        (tagged with @p tag) and returns false; on success it writes the
     *        validated copy into @p out_cc and returns true.
     *
     *        @p Config must expose `comm_config_version`,
     *        `descriptor_layout_version`, and `extension_flags` — every mesh
     *        CommConfig does, and a future one that doesn't fails to compile here.
     */
    template <class Config>
    bool validateMeshCommConfig(const char*    tag,
                                const void*    param,
                                std::size_t    param_size,
                                std::uint32_t  expected_comm_version,
                                std::uint32_t  expected_layout_version,
                                std::uint32_t  known_ext_flags,
                                Config&        out_cc)
    {
        if (param == nullptr || param_size != sizeof(Config))
        {
            std::cerr << "[" << tag << "] invalid payload size: expected "
                      << sizeof(Config) << ", got " << param_size << std::endl;
            return false;
        }

        out_cc = *static_cast<const Config*>(param);

        if (out_cc.comm_config_version != expected_comm_version)
        {
            std::cerr << "[" << tag << "] unsupported comm config version: "
                      << out_cc.comm_config_version << " (expected "
                      << expected_comm_version << ")" << std::endl;
            return false;
        }

        if (out_cc.descriptor_layout_version != expected_layout_version)
        {
            std::cerr << "[" << tag << "] unsupported descriptor layout version: "
                      << out_cc.descriptor_layout_version << " (expected "
                      << expected_layout_version << ")" << std::endl;
            return false;
        }

        if ((out_cc.extension_flags & ~known_ext_flags) != 0u)
        {
            std::cerr << "[" << tag << "] unknown extension flags: "
                      << out_cc.extension_flags << std::endl;
            return false;
        }

        return true;
    }

} // namespace lux::render
