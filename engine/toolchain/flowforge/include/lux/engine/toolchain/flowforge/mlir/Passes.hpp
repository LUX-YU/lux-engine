#pragma once
//
// Passes.hpp — Lowering pipeline + JIT entrypoints for the FlowForge MLIR
// compiler. The flow is:
//
//   FlowGraph
//       |  MLIRBuilder::generateIR        (IR.hpp)
//       v
//   FlowForge dialect IR (one func.func per graph entry: @main + functions)
//       |  createFlowForgeToCFPass / lowerToCF
//       v
//   cf + func + arith dialect IR (basic blocks + branches)
//       |  lowerToLLVM
//       v
//   llvm dialect IR
//       |  runMainJIT  (or ExecutionEngine directly)
//       v
//   native machine code
//
#include <memory>
#include <vector>
#include <lux/engine/toolchain/flowforge/visibility.h>
#include "IR.hpp"

namespace mlir { class Pass; class ModuleOp; }

namespace lux::flowforge
{
    // Lowers FlowForge dialect ops to the cf dialect (basic blocks +
    // branches) plus func/arith. The token type is erased entirely (it
    // carries no LLVM-level meaning — it only exists in the FlowForge
    // dialect to enforce execution ordering between control ops, which
    // becomes implicit in the block structure).
    //
    // cf (not scf) is the target because flow graphs have unstructured
    // exits — Return anywhere and Break out of loops — which scf cannot
    // express without unwind-flag threading; in cf they are single jumps.
    //
    // Op-by-op rewrite:
    //   flowforge.branch      -> cf.cond_br + inlined legs + join block
    //   flowforge.for_loop    -> preheader/header/body/exit blocks
    //                            (iv as header block-arg, step = 1,
    //                             exclusive upper bound)
    //   flowforge.while_loop  -> cond block(s) + body block(s); the cond
    //                            region re-evaluates the loop condition
    //                            every iteration
    //   flowforge.yield       -> cf.br (join / latch / cond re-entry)
    //   flowforge.cond_yield  -> cf.cond_br (body vs exit)
    //   flowforge.break       -> cf.br to the innermost loop's exit block
    //   flowforge.return      -> cf.br to the function's exit block, which
    //                            func.returns the forwarded values
    //   flowforge.token_merge -> replaced by its first input
    //   flowforge.start       -> erased
    LUX_TOOLCHAIN_FLOWFORGE_PUBLIC std::unique_ptr<mlir::Pass> createFlowForgeToCFPass();

    // Runs the FlowForge -> CF pass on the IR's module in place.
    [[nodiscard]] LUX_TOOLCHAIN_FLOWFORGE_PUBLIC FlowForgeResult<void> lowerToCF(IR& ir);

    // Drives the full pipeline: FlowForge -> CF -> LLVM dialect.
    // After this returns, the IR's module contains only `llvm.*` ops (plus
    // the builtin module), ready for `mlir::ExecutionEngine` JIT or LLVM IR
    // emission via `translateModuleToLLVMIR`.
    [[nodiscard]] LUX_TOOLCHAIN_FLOWFORGE_PUBLIC FlowForgeResult<void> lowerToLLVM(IR& ir);

    // One-shot JIT: lowers the IR all the way down (if not already) and
    // runs `@main`. Returns the function's i32 return value (0 if `@main`
    // is void).
    // The host LLVM target is registered idempotently inside this call.
    //
    // For tests / quick experiments. Production callers should drive the
    // pipeline manually (lowerToLLVM + ExecutionEngine::create + lookup)
    // to register native symbols, control optimization level, etc.
    [[nodiscard]] LUX_TOOLCHAIN_FLOWFORGE_PUBLIC FlowForgeResult<int> runMainJIT(IR& ir);

    /// A host function exposed to the JIT under the extern name a
    /// NATIVE_FUNC_CALL node declared (its RefInvokable::name). The address
    /// must stay valid for the duration of the call.
    struct JitNativeSymbol
    {
        const char* name    = nullptr;
        void*       address = nullptr;
    };

    // Symbol-binding flavor of runMainJIT: registers the given host functions
    // with the ExecutionEngine before invoking `@main`, so graphs containing
    // NATIVE_FUNC_CALL nodes resolve their extern declarations at JIT time
    // (the engine does NOT search the host process's exports on Windows).
    // This is the editor run-button path.
    [[nodiscard]] LUX_TOOLCHAIN_FLOWFORGE_PUBLIC FlowForgeResult<int> runMainJIT(
        IR&                                 ir,
        const std::vector<JitNativeSymbol>& native_symbols
    );
}
