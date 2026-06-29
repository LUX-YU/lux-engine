#pragma once
//
// Passes.hpp — Lowering pipeline + JIT entrypoints for the FlowForge MLIR
// compiler. The flow is:
//
//   FlowGraph
//       |  MLIRBuilder::generateIR        (IR.hpp)
//       v
//   FlowForge dialect IR (wrapped in func.func @main)
//       |  createFlowForgeToSCFPass / lowerToSCF
//       v
//   scf + func + arith dialect IR
//       |  lowerToLLVM
//       v
//   llvm dialect IR
//       |  runMainJIT  (or ExecutionEngine directly)
//       v
//   native machine code
//
#include <memory>
#include <vector>
#include <lux/engine/editor/visibility.h>

namespace mlir { class Pass; class ModuleOp; }

namespace lux::flowforge
{
    class IR;

    // Lowers FlowForge dialect ops to standard MLIR dialects (scf + func +
    // arith). The token type is erased entirely (it carries no LLVM-level
    // meaning — it only exists in the FlowForge dialect to enforce execution
    // ordering between control ops, which becomes implicit in the SSA
    // structure of scf/func/arith).
    //
    // Op-by-op rewrite:
    //   flowforge.start       -> erased  (token vanishes; consumers retire
    //                                     via the patterns below)
    //   flowforge.token_merge -> replaced by its first input (only one input
    //                                     is live at runtime, the SSA value
    //                                     is irrelevant once tokens are gone)
    //   flowforge.branch      -> scf.if    (no results; the two per-leg
    //                                     tokens disappear)
    //   flowforge.for_loop    -> scf.for   (induction var preserved; tokens
    //                                     dropped; step = 1)
    //   flowforge.while_loop  -> scf.while (cond region holds the loop guard,
    //                                     body region holds the body; tokens
    //                                     dropped)
    //   flowforge.sequence    -> ops inlined into parent region in order
    //                                     (the single body region's contents
    //                                     are spliced into the parent; the
    //                                     fan-out is purely a no-op at the
    //                                     scf level since each leg already
    //                                     ran sequentially in the body)
    //   flowforge.yield       -> scf.yield (token operands filtered out)
    //   flowforge.cond_yield  -> scf.condition (drops the token; the cond
    //                                     becomes the scf.while guard)
    //   flowforge.return      -> func.return (drops the token; data operands
    //                                     pass through as the function's
    //                                     return values)
    LUX_EDITOR_PUBLIC std::unique_ptr<mlir::Pass> createFlowForgeToSCFPass();

    // Runs the FlowForge -> SCF pass on the IR's module in place. Throws
    // std::runtime_error on pass failure. For tests / consumers that want a
    // one-shot lowering without assembling a PassManager themselves.
    LUX_EDITOR_PUBLIC void lowerToSCF(IR& ir);

    // Drives the full pipeline: FlowForge -> SCF -> CF -> LLVM dialect.
    // After this returns, the IR's module contains only `llvm.*` ops (plus
    // the builtin module), ready for `mlir::ExecutionEngine` JIT or LLVM IR
    // emission via `translateModuleToLLVMIR`. Throws std::runtime_error on
    // any pass failure.
    LUX_EDITOR_PUBLIC void lowerToLLVM(IR& ir);

    // One-shot JIT: lowers the IR all the way down (if not already) and
    // runs `@main`. Returns the function's i32 return value (0 if `@main`
    // is void). Throws std::runtime_error if the lowering or JIT fails.
    // The host LLVM target is registered idempotently inside this call.
    //
    // For tests / quick experiments. Production callers should drive the
    // pipeline manually (lowerToLLVM + ExecutionEngine::create + lookup)
    // to register native symbols, control optimization level, etc.
    LUX_EDITOR_PUBLIC int runMainJIT(IR& ir);

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
    LUX_EDITOR_PUBLIC int runMainJIT(IR& ir,
                                     const std::vector<JitNativeSymbol>& native_symbols);
}
