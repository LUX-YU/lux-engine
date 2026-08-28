#pragma once
//
// AOT.hpp — cook-time compilation of a FlowGraph into a native shared
// library that speaks the lux_script_abi plugin contract.
//
// The produced DLL is indistinguishable from a hand-written native plugin:
//
//   * it exports lux_script_get_module() — a module descriptor whose
//     function table carries one call_frame entry per OnEvent node (the
//     table's function name is the event's DISPLAY name, so
//     NativeModule::findFunction("Tick") resolves at bind time);
//   * when the graph calls into the engine it also exports
//     lux_script_bind_host() — every native / reflected call site compiles
//     to an indirect call through an internal import-slot table that
//     bind_host fills via the host resolver (same by-name model the JIT
//     path uses, bound at load instead of at materialization);
//   * the per-instance state block arrives through call_frame.user_context;
//     its size / defaults / layout hash are captured in AotArtifact so the
//     cook step can serialize them into the script manifest.
//
// MLIR / LLVM stay entirely cook-side (inside this compiler target): the
// runtime loads the DLL through the concrete NativeModule API with zero
// compiler dependency.
//
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <lux/engine/flowforge/compiler/IR.hpp>
#include <lux/engine/description/Script.hpp>

namespace lux::flowforge
{
    class FlowGraph;
    class IRContext;

    struct AotArtifact
    {
        std::vector<std::byte>    object;      ///< native object (COFF) bytes
        std::string               module_name;
        std::vector<std::string>  imports;     ///< host symbols bind_host resolves
        std::vector<lux::rdesc::ScriptFunction> exports;

        // Instance-state recipe (mirrors the JIT path's StateLayout): the
        // host allocates state_size bytes per instance, copies the defaults
        // in, and passes the base pointer as call_frame.user_context.
        uint64_t                  state_size = 0;
        uint64_t                  state_hash = 0;
        uint32_t                  state_align = 1;
        std::vector<std::byte>    state_defaults;
    };

    /// Graph -> FlowForge MLIR -> LLVM -> native object.
    [[nodiscard]] FlowForgeResult<AotArtifact> compileToObject(
        IRContext& context,
        const FlowGraph& graph,
        const FlowForgeCompileOptions& options
    ) noexcept;

    /// Writes artifact.object next to out_dll (same stem, ".obj") and links
    /// it into a shared library at out_dll. Cook-time only — spawns the
    /// linker as an external process.
    [[nodiscard]] FlowForgeResult<void> linkSharedLibrary(
        const AotArtifact& artifact,
        const std::filesystem::path& out_dll,
        const FlowForgeCompileOptions& options
    ) noexcept;
}
