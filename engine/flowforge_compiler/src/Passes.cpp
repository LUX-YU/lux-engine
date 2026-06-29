//===========================================================================
// Passes.cpp — FlowForge -> SCF -> LLVM lowering pipeline + JIT runner.
//
// Erases the !flowforge.token type entirely (it's a dialect-only ordering
// artefact with no LLVM-level meaning) and rewrites each flowforge op into
// its scf / func / arith equivalent. Operates on the @main function
// emitted by MLIRBuilder::generateIR.
//
// Order of rewrites matters because tokens form a chain that the runtime
// scaffold relies on:
//   1. flowforge.return  -> func.return (drops the token operand). After
//      this, the chain's terminal consumer is gone.
//   2. flowforge.token_merge -> erased (its uses are now gone from #1).
//   3. Control ops in POST-ORDER (innermost first): each erases its sub-
//      region tokens before the parent rewrite needs to. By the time we
//      rewrite the outer op, all inner ops are scf and the outer's
//      block-arg token has no uses.
//   4. flowforge.start -> erased (no consumers left).
//===========================================================================
#include "lux/engine/flowforge/mlir/Passes.hpp"
#include "lux/engine/flowforge/mlir/IRImpl.hpp"
#include "lux/engine/flowforge/mlir/IR.hpp"
#include "FlowForgeDialect.h"
#include "FlowForgeVersionCompat.h"
#include FLOWFORGE_ARITH_DIALECT_INCLUDE

#include <mlir/Dialect/SCF/IR/SCF.h>
#include <mlir/Dialect/Func/IR/FuncOps.h>
#include <mlir/Dialect/ControlFlow/IR/ControlFlow.h>
#include <mlir/IR/Builders.h>
#include <mlir/IR/BuiltinOps.h>
#include <mlir/IR/IRMapping.h>
#include <mlir/IR/PatternMatch.h>
#include <mlir/Pass/Pass.h>
#include <mlir/Pass/PassManager.h>
#include <llvm/ADT/SmallVector.h>

// Standard MLIR conversion + JIT plumbing.
#include <mlir/Conversion/SCFToControlFlow/SCFToControlFlow.h>
#include <mlir/Conversion/ArithToLLVM/ArithToLLVM.h>
#include <mlir/Conversion/ControlFlowToLLVM/ControlFlowToLLVM.h>
#include <mlir/Conversion/FuncToLLVM/ConvertFuncToLLVMPass.h>
#include <mlir/Conversion/ReconcileUnrealizedCasts/ReconcileUnrealizedCasts.h>
#include <mlir/ExecutionEngine/ExecutionEngine.h>
#include <mlir/Target/LLVMIR/Dialect/Builtin/BuiltinToLLVMIRTranslation.h>
#include <mlir/Target/LLVMIR/Dialect/LLVMIR/LLVMToLLVMIRTranslation.h>
#include <llvm/Support/TargetSelect.h>

#include <stdexcept>

namespace lf = mlir::flowforge;

namespace lux::flowforge
{
namespace
{
    // Splice `srcBlock`'s non-terminator ops into `dstBlock` immediately before
    // `insertBefore`. The source block's arguments must have no remaining uses
    // (otherwise the moved ops would reference a value that's about to die).
    //
    // The terminator (flowforge.yield / cond_yield) is NOT moved — caller is
    // responsible for emitting the equivalent scf terminator (scf.yield /
    // scf.condition) into dstBlock before calling this if the destination
    // region requires a terminator.
    static void spliceBlockOps(mlir::Block& srcBlock,
                               mlir::Block& dstBlock,
                               mlir::Operation* insertBefore)
    {
        while (!srcBlock.empty()) {
            auto& op = srcBlock.front();
            if (&op == srcBlock.getTerminator())
                break;
            op.moveBefore(insertBefore);
        }
    }

    // Rewrite `flowforge.return` -> `func.return`. Drops the token operand;
    // remaining operands (variadic $rets, the function's return values) pass
    // through unchanged.
    static void lowerReturnOp(lf::ReturnOp op)
    {
        mlir::OpBuilder b(op);
        b.create<mlir::func::ReturnOp>(op.getLoc(), op.getRets());
        op.erase();
    }

    // Rewrite `flowforge.branch` -> `scf.if`. Inputs: drop the token, keep the
    // cond. Results: dropped (the two per-leg tokens are gone). Then/else
    // region bodies are spliced from the source op's regions into the
    // newly-created scf.if's default region blocks (which already contain a
    // trailing scf.yield from the IfOp builder).
    //
    // Critically: outer consumers of the branch's two token results (typically
    // a flowforge.token_merge or another flowforge.yield) still reference
    // those results when this runs in post-order. Retarget those uses to the
    // op's INPUT token before erasing — semantically identical since the
    // token type itself is going away, and the input token's SSA defining op
    // dominates anywhere the result is used.
    static void lowerBranchOp(lf::BranchOp op)
    {
        op.getTrueExec().replaceAllUsesWith(op.getIn());
        op.getFalseExec().replaceAllUsesWith(op.getIn());

        mlir::OpBuilder b(op);
        auto loc = op.getLoc();

        auto ifOp = b.create<mlir::scf::IfOp>(
            loc, mlir::TypeRange{}, op.getCond(),
            /*withElseRegion=*/true);

        auto& srcThen = op.getThenRegion().front();
        auto& dstThen = ifOp.getThenRegion().front();
        spliceBlockOps(srcThen, dstThen, dstThen.getTerminator());

        auto& srcElse = op.getElseRegion().front();
        auto& dstElse = ifOp.getElseRegion().front();
        spliceBlockOps(srcElse, dstElse, dstElse.getTerminator());

        op.erase();
    }

    // Rewrite `flowforge.for_loop` -> `scf.for`. Drops the token; keeps
    // %first / %last as bounds, synthesises a step of 1. Body region: the
    // induction-variable block-arg carries through (scf.for's IV); the token
    // block-arg is dropped (which is safe because no remaining op uses it).
    //
    // Outer token-result consumers are retargeted to the op's input token
    // (same rationale as lowerBranchOp). The iv result (op.getResult(2),
    // post-loop final IV) is currently expected to be unused — FlowGraph
    // exposes the per-iteration IV via the body block-arg, not via a
    // post-loop pin. If a future graph wires it, we'd need to materialize a
    // post-loop constant (e.g. op.getLast()), but for today we fail loudly.
    static void lowerForLoopOp(lf::ForLoopOp op)
    {
        op.getBodyExec().replaceAllUsesWith(op.getIn());
        op.getDoneExec().replaceAllUsesWith(op.getIn());
        if (!op.getIv().use_empty()) {
            op.emitError(
                "flowforge.for_loop post-loop iv has uses — not yet supported "
                "by FlowForge -> SCF lowering");
            return;  // leftover-check at end of pass will signalPassFailure()
        }

        mlir::OpBuilder b(op);
        auto loc = op.getLoc();
        auto step = b.create<mlir::arith::ConstantIndexOp>(loc, 1);

        auto forOp = b.create<mlir::scf::ForOp>(
            loc, op.getFirst(), op.getLast(), step,
            /*iterArgs=*/mlir::ValueRange{});

        auto& srcBody = op.getBody().front();
        auto& dstBody = forOp.getRegion().front();
        // dstBody comes with one block:
        //   ^bb0(%iv: index):
        //     scf.yield
        // The IV in the source body was block-arg(1); in dst it's block-arg(0).
        srcBody.getArgument(1).replaceAllUsesWith(dstBody.getArgument(0));

        spliceBlockOps(srcBody, dstBody, dstBody.getTerminator());

        op.erase();
    }

    // Rewrite `flowforge.while_loop` -> `scf.while`. The flowforge dialect
    // models WhileLoop with:
    //   - cond region (args: token, i1): terminates with flowforge.cond_yield
    //     (passes the cond through unchanged — no per-iteration cond
    //     expression in FlowGraph today, the initial cond is the loop's
    //     lifetime cond)
    //   - body region (args: token): does work, terminates with flowforge.yield
    // We lower to scf.while with zero iter-args and zero results. The cond
    // is an outer SSA value (op.getCond()) accessible inside the before-region
    // via MLIR's enclosing-region dominance.
    static void lowerWhileLoopOp(lf::WhileLoopOp op)
    {
        op.getBodyExec().replaceAllUsesWith(op.getIn());
        op.getDoneExec().replaceAllUsesWith(op.getIn());

        mlir::OpBuilder b(op);
        auto loc = op.getLoc();

        auto whileOp = b.create<mlir::scf::WhileOp>(
            loc, mlir::TypeRange{}, mlir::ValueRange{});

        // Before-region: ^bb0: scf.condition(%outer_cond)
        {
            mlir::OpBuilder::InsertionGuard guard(b);
            b.createBlock(&whileOp.getBefore(), whileOp.getBefore().end(),
                          mlir::TypeRange{}, mlir::ArrayRef<mlir::Location>{});
            b.create<mlir::scf::ConditionOp>(
                loc, op.getCond(), mlir::ValueRange{});
        }

        // After-region: ^bb0: ... body ops ... scf.yield
        auto& srcBody = FLOWFORGE_GET_BODY_REGION_WHILE(op).front();
        {
            mlir::OpBuilder::InsertionGuard guard(b);
            auto* afterBlk = b.createBlock(
                &whileOp.getAfter(), whileOp.getAfter().end(),
                mlir::TypeRange{}, mlir::ArrayRef<mlir::Location>{});
            auto yield = b.create<mlir::scf::YieldOp>(loc);
            spliceBlockOps(srcBody, *afterBlk, yield);
        }

        op.erase();
    }

    // Rewrite `flowforge.sequence` — inline its body's ops directly into the
    // parent region. SequenceOp had a single body region whose contents are
    // already in sequential order (lowerChain inlined every leg into one
    // block). The op itself has no LLVM-level meaning past that ordering, so
    // we just retarget result uses to the input token, splice the body, and
    // erase.
    static void lowerSequenceOp(lf::SequenceOp op)
    {
        for (mlir::Value r : op.getResults())
            r.replaceAllUsesWith(op.getIn());

        auto& srcBlk = op.getRegion().front();
        // Splice into parent block, before the SequenceOp itself.
        mlir::Block* parentBlk = op->getBlock();
        spliceBlockOps(srcBlk, *parentBlk, op);
        op.erase();
    }

    class FlowForgeToSCFPass
        : public mlir::PassWrapper<FlowForgeToSCFPass,
                                   mlir::OperationPass<mlir::ModuleOp>>
    {
    public:
        MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(FlowForgeToSCFPass)

        llvm::StringRef getArgument() const final
            { return "flowforge-to-scf"; }
        llvm::StringRef getDescription() const final
            { return "Lower flowforge dialect ops to scf + func"; }

        void getDependentDialects(mlir::DialectRegistry& reg) const override
        {
            reg.insert<mlir::scf::SCFDialect, mlir::func::FuncDialect,
                       mlir::arith::ArithDialect>();
        }

        void runOnOperation() override
        {
            auto module = getOperation();

            // Step 1. flowforge.return -> func.return. This drops the token
            // operand and removes the chain's last consumer.
            module.walk([](lf::ReturnOp op) { lowerReturnOp(op); });

            // Step 2. flowforge.token_merge: replace uses with first operand,
            // erase. Since #1 already dropped the only token consumer
            // (return), token_merge has no live uses other than within other
            // soon-to-be-erased flowforge ops (its operands flow into
            // control-op block-args via dataflow only, which the control-op
            // lowering handles). RAUW-with-operand-0 is safe because the
            // merge is semantic — exactly one input is "live", the SSA
            // value doesn't matter once the token type is going away.
            module.walk([](lf::TokenMergeOp op) {
                op.getMerged().replaceAllUsesWith(op.getInputs().front());
                op.erase();
            });

            // Step 3. Control ops in POST-ORDER (innermost first), so that
            // by the time we rewrite the outer op, all inner flowforge ops
            // are already scf — the outer's region block-arg (the token)
            // has no remaining uses.
            llvm::SmallVector<mlir::Operation*, 16> control_ops;
            module.walk<mlir::WalkOrder::PostOrder>(
                [&](mlir::Operation* op) {
                    if (mlir::isa<lf::BranchOp, lf::ForLoopOp,
                                  lf::WhileLoopOp, lf::SequenceOp>(op))
                        control_ops.push_back(op);
                });
            for (auto* op : control_ops) {
                if (auto br = mlir::dyn_cast<lf::BranchOp>(op))
                    lowerBranchOp(br);
                else if (auto fl = mlir::dyn_cast<lf::ForLoopOp>(op))
                    lowerForLoopOp(fl);
                else if (auto wl = mlir::dyn_cast<lf::WhileLoopOp>(op))
                    lowerWhileLoopOp(wl);
                else if (auto sq = mlir::dyn_cast<lf::SequenceOp>(op))
                    lowerSequenceOp(sq);
            }

            // Step 4. flowforge.start: no more consumers, erase.
            module.walk([](lf::StartOp op) {
                if (op.getResult().use_empty())
                    op.erase();
            });

            // Step 5. Any leftover flowforge ops at this point indicate the
            // graph contained a shape this pass doesn't handle yet — fail
            // explicitly so the test surfaces the gap.
            mlir::WalkResult leftover = module.walk(
                [&](mlir::Operation* op) -> mlir::WalkResult {
                    if (op->getDialect()
                        && op->getDialect()->getNamespace() == "flowforge") {
                        op->emitError("FlowForge op survived lowering pass: ")
                            << op->getName();
                        return mlir::WalkResult::interrupt();
                    }
                    return mlir::WalkResult::advance();
                });
            if (leftover.wasInterrupted())
                signalPassFailure();
        }
    };
} // anonymous

std::unique_ptr<mlir::Pass> createFlowForgeToSCFPass()
{
    return std::make_unique<FlowForgeToSCFPass>();
}

void lowerToSCF(IR& ir)
{
    auto module = ir.impl().top_module;
    if (!module) throw std::runtime_error("lowerToSCF: empty IR module");
    mlir::PassManager pm(module.getContext());
    pm.addPass(createFlowForgeToSCFPass());
    if (mlir::failed(pm.run(module)))
        throw std::runtime_error("lowerToSCF: FlowForge -> SCF pass failed");
}

void lowerToLLVM(IR& ir)
{
    auto module = ir.impl().top_module;
    if (!module) throw std::runtime_error("lowerToLLVM: empty IR module");
    mlir::PassManager pm(module.getContext());

    // Stage 1 — FlowForge -> SCF (idempotent if already lowered: the
    // leftover-check at the end of the pass is a no-op when there's
    // nothing to do).
    pm.addPass(createFlowForgeToSCFPass());

    // Stage 2 — scf -> cf (structured control flow expanded to basic blocks
    // with branches).
    pm.addPass(mlir::createConvertSCFToCFPass());

    // Stage 3 — dialect-to-LLVM lowerings. Order matters: each one only
    // converts ops of its source dialect, and ConvertFuncToLLVM rewrites
    // function signatures which the others' converted ops then plug into.
    pm.addPass(mlir::createArithToLLVMConversionPass());
    pm.addPass(mlir::createConvertControlFlowToLLVMPass());
    pm.addPass(mlir::createConvertFuncToLLVMPass());

    // Stage 4 — sweep out any builtin.unrealized_conversion_cast ops that
    // remained as bridges between the per-pass dialect rewrites (e.g. the
    // index<->i64 round-trip scf-to-cf left behind for the loop IV).
    // Without this, LLVM IR translation fails on unrealized_conversion_cast.
    pm.addPass(mlir::createReconcileUnrealizedCastsPass());

    if (mlir::failed(pm.run(module)))
        throw std::runtime_error("lowerToLLVM: lowering pipeline failed");
}

int runMainJIT(IR& ir)
{
    return runMainJIT(ir, {});
}

int runMainJIT(IR& ir, const std::vector<JitNativeSymbol>& native_symbols)
{
    auto module = ir.impl().top_module;
    if (!module) throw std::runtime_error("runMainJIT: empty IR module");

    // Idempotently make sure the host LLVM target is registered. Calling
    // this from a translation unit that statically links the X86 codegen
    // libs (via the flowforge_compiler dialect target) guarantees the X86
    // init code is actually pulled into the binary, regardless of whether
    // the caller's main() already did it.
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();

    // Make sure the LLVM dialect's MLIR -> LLVM IR translation is registered
    // on the context — ExecutionEngine needs it to emit IR for codegen.
    auto* ctx = module.getContext();
    mlir::registerBuiltinDialectTranslation(*ctx);
    mlir::registerLLVMDialectTranslation(*ctx);

    lowerToLLVM(ir);

    // O2 codegen: same LLVM backend as AOT — the default (O0) leaves easy
    // performance on the table for editor preview runs.
    mlir::ExecutionEngineOptions engine_options;
    engine_options.jitCodeGenOptLevel = llvm::CodeGenOptLevel::Default;

    auto maybeEngine = mlir::ExecutionEngine::create(module, engine_options);
    if (!maybeEngine)
        throw std::runtime_error(
            "runMainJIT: ExecutionEngine::create failed: "
            + llvm::toString(maybeEngine.takeError()));
    auto& engine = *maybeEngine;

    // Bind host functions for the graph's NATIVE_FUNC_CALL extern
    // declarations — ORC does not search the host process's exports on
    // Windows, so unresolved symbols would fail at materialization.
    if (!native_symbols.empty())
    {
        engine->registerSymbols(
            [&](llvm::orc::MangleAndInterner mangle)
            {
                llvm::orc::SymbolMap map;
                for (const JitNativeSymbol& sym : native_symbols)
                {
                    if (!sym.name || !sym.address) continue;
                    map[mangle(sym.name)] = llvm::orc::ExecutorSymbolDef{
                        llvm::orc::ExecutorAddr::fromPtr(sym.address),
                        llvm::JITSymbolFlags::Exported};
                }
                return map;
            });
    }

    auto expectedFn = engine->lookup("main");
    if (!expectedFn)
        throw std::runtime_error(
            "runMainJIT: lookup(main) failed: "
            + llvm::toString(expectedFn.takeError()));

    // Today `@main` is void (ReturnNode exposes no DataInPin). When that
    // gap closes the function type will be derived from those pins; this
    // helper accepts both `void()` and `int32_t()` signatures by JIT-
    // probing the function as `void()` and returning 0 in the void case.
    // Production callers should drive ExecutionEngine themselves with the
    // signature they expect.
    auto* fnVoid = reinterpret_cast<void(*)()>(expectedFn.get());
    fnVoid();
    return 0;
}

} // namespace lux::flowforge
