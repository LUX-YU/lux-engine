#pragma once
// ============================================================================
//  ShaderResourceOperation.hpp — Shader module compile / destroy over comm
// ============================================================================

#include <lux/engine/function/render/client/protocol/RenderCommTypes.hpp>
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>
#include <lux/engine/function/render/client/resources/ops/ResourceOperationCommon.hpp> // DestroyResourcePayload<>

#include <cstdint>
#include <type_traits>

namespace lux::render
{
    namespace type_ids
    {
        inline constexpr TypeId CompileShader = 12;
        inline constexpr TypeId DestroyShader = 13;
    } // namespace type_ids

    // ── Payloads ──────────────────────────────────────────────────────

    /// Client → Server: compile SPIR-V into a VkShaderModule.
    /// The server stores the resulting ShaderHandle in its registry and
    /// returns a ShaderHandle via ShaderCompiledReply.
    struct CompileShaderPayload
    {
        ExternalDataRef spirv_data{};       ///< raw SPIR-V bytes
        ExternalDataRef shader_info_data{}; ///< ShaderInfo::serialize() output
    };
    static_assert(std::is_trivially_copyable_v<CompileShaderPayload>);

    /// Client → Server: destroy a previously compiled shader.
    using DestroyShaderPayload = DestroyResourcePayload<ShaderHandle>;
    static_assert(std::is_trivially_copyable_v<DestroyShaderPayload>);

} // namespace lux::render
