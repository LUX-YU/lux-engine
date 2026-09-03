//===========================================================================
// Passes.cpp — FlowForge -> CF -> LLVM lowering pipeline + JIT runner.
//
// The FlowForge dialect is STRUCTURED (region-based) so the builder and the
// verifier can reason about the graph; the lowering target is the cf
// dialect (basic blocks + branches) rather than scf, because a flow graph
// has unstructured exits — Return anywhere, Break out of loops — that scf
// cannot express without invasive unwind-flag threading. In cf they are a
// single jump.
//
// Rewrite order inside the pass:
//   1. Control ops in POST-ORDER (innermost first): each op's regions are
//      inlined into the parent block sequence around a split point;
//      flowforge.yield terminators become jumps (join block / loop latch /
//      cond re-entry), flowforge.break becomes a jump to the loop's exit,
//      and flowforge.return terminators are left in place (handled in #3).
//   2. flowforge.token_merge -> replaced by its first operand (tokens are
//      pure ordering artefacts; the SSA value is irrelevant once erased).
//   3. Per function: an exit block is appended (func.return of the exit
//      block's arguments); every flowforge.return becomes cf.br ^exit(rets).
//   4. flowforge.start ops are erased (all token consumers are gone).
//   5. Any surviving flowforge op fails the pass loudly.
//===========================================================================
#include "lux/engine/flowforge/compiler/Passes.hpp"
#include "lux/engine/flowforge/compiler/IRImpl.hpp"
#include "lux/engine/flowforge/compiler/IR.hpp"
#include "FlowForgeDialect.h"
#include "FlowForgeVersionCompat.h"
#include FLOWFORGE_ARITH_DIALECT_INCLUDE

#include <mlir/Dialect/Func/IR/FuncOps.h>
#include <mlir/Dialect/ControlFlow/IR/ControlFlowOps.h>
#include <mlir/IR/Builders.h>
#include <mlir/IR/BuiltinOps.h>
#include <mlir/IR/PatternMatch.h>
#include <mlir/Pass/Pass.h>
#include <mlir/Pass/PassManager.h>
#include <mlir/Transforms/RegionUtils.h>
#include <llvm/ADT/SmallVector.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <new>
#include <vector>

// Standard MLIR conversion + JIT plumbing.
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
namespace cf = mlir::cf;

namespace lux::flowforge
{
    namespace
    {
        // Collect every terminator of type OpT across all blocks of `region`.
        // (Post-order conversion guarantees these belong to the region's op —
        // inner control ops were already rewritten and their terminators are
        // cf jumps by now.)
        template <typename OpT> llvm::SmallVector<OpT, 4> collectTerminators(mlir::Region& region)
        {
            llvm::SmallVector<OpT, 4> found;
            for (mlir::Block& b : region)
                if (!b.empty())
                    if (auto t = mlir::dyn_cast<OpT>(b.getTerminator()))
                        found.push_back(t);
            return found;
        }

        // Inline all blocks of `region` into `parent` immediately before
        // `before`. The region is left empty.
        void inlineRegionBefore(mlir::Region& region, mlir::Region& parent, mlir::Block* before)
        {
            parent.getBlocks().splice(before->getIterator(), region.getBlocks());
        }

        // Rewrite `flowforge.branch`:
        //   ^cond: ... cf.cond_br %c, ^then, ^else
        //   ^then/^else: (inlined leg blocks; yields -> cf.br ^join)
        //   ^join: (everything that followed the branch op)
        // Outer consumers of the two token results are retargeted to the input
        // token first (tokens carry no data — only ordering, which the block
        // structure now encodes).
        void lowerBranchToCF(lf::BranchOp op)
        {
            op.getTrueExec().replaceAllUsesWith(op.getIn());
            op.getFalseExec().replaceAllUsesWith(op.getIn());

            mlir::Block* condBlock = op->getBlock();
            mlir::Region* parent = condBlock->getParent();
            mlir::Block* join = condBlock->splitBlock(op);
            auto loc = op.getLoc();

            auto prepareLeg = [&](mlir::Region& region) -> mlir::Block* {
                mlir::Block* entry = &region.front();
                for (lf::YieldOp y : collectTerminators<lf::YieldOp>(region))
                {
                    mlir::OpBuilder b(y);
                    b.create<cf::BranchOp>(loc, join);
                    y.erase();
                }
                entry->getArgument(0).replaceAllUsesWith(op.getIn());
                entry->eraseArgument(0);
                inlineRegionBefore(region, *parent, join);
                return entry;
            };
            mlir::Block* thenEntry = prepareLeg(op.getThenRegion());
            mlir::Block* elseEntry = prepareLeg(op.getElseRegion());

            mlir::OpBuilder b(condBlock, condBlock->end());
            b.create<cf::CondBranchOp>(loc, op.getCond(), thenEntry, mlir::ValueRange{}, elseEntry, mlir::ValueRange{});
            op.erase();
        }

        // Rewrite `flowforge.for_loop`:
        //   ^preheader: ... cf.br ^header(%first)
        //   ^header(%iv: index): %c = cmpi slt %iv, %last; cf.cond_br %c, ^body(%iv), ^exit
        //   ^body(%iv_b): (inlined; yields -> %iv_b+1, cf.br ^header; breaks -> cf.br ^exit)
        //   ^exit: (everything after the loop)
        void lowerForLoopToCF(lf::ForLoopOp op)
        {
            op.getBodyExec().replaceAllUsesWith(op.getIn());
            op.getDoneExec().replaceAllUsesWith(op.getIn());
            if (!op.getIv().use_empty())
            {
                op.emitError("flowforge.for_loop post-loop iv has uses — not yet supported "
                             "by the FlowForge -> CF lowering");
                return; // leftover-check at end of pass will signalPassFailure()
            }

            mlir::Block* preheader = op->getBlock();
            mlir::Region* parent = preheader->getParent();
            mlir::Block* exit = preheader->splitBlock(op);
            auto loc = op.getLoc();

            mlir::OpBuilder ctxb(op.getContext());
            mlir::Block* header = new mlir::Block();
            header->addArgument(ctxb.getIndexType(), loc);
            parent->getBlocks().insert(exit->getIterator(), header);

            mlir::Region& bodyRegion = op.getBody();
            mlir::Block* bodyEntry = &bodyRegion.front();
            bodyEntry->getArgument(0).replaceAllUsesWith(op.getIn());
            bodyEntry->eraseArgument(0);
            mlir::Value bodyIv = bodyEntry->getArgument(0);

            for (lf::YieldOp y : collectTerminators<lf::YieldOp>(bodyRegion))
            {
                mlir::OpBuilder b(y);
                auto one = b.create<mlir::arith::ConstantIndexOp>(loc, 1);
                auto next = b.create<mlir::arith::AddIOp>(loc, bodyIv, one);
                b.create<cf::BranchOp>(loc, header, mlir::ValueRange{next});
                y.erase();
            }
            for (lf::BreakOp brk : collectTerminators<lf::BreakOp>(bodyRegion))
            {
                mlir::OpBuilder b(brk);
                b.create<cf::BranchOp>(loc, exit);
                brk.erase();
            }
            inlineRegionBefore(bodyRegion, *parent, exit);

            {
                mlir::OpBuilder b(header, header->end());
                mlir::Value iv = header->getArgument(0);
                mlir::Value cmp = b.create<mlir::arith::CmpIOp>(loc, mlir::arith::CmpIPredicate::slt, iv, op.getLast());
                b.create<cf::CondBranchOp>(loc, cmp, bodyEntry, mlir::ValueRange{iv}, exit, mlir::ValueRange{});
            }
            {
                mlir::OpBuilder b(preheader, preheader->end());
                b.create<cf::BranchOp>(loc, header, mlir::ValueRange{op.getFirst()});
            }
            op.erase();
        }

        // Rewrite `flowforge.while_loop`:
        //   ^preheader: ... cf.br ^cond
        //   ^cond: (inlined cond region; cond_yield -> cf.cond_br %c, ^body, ^exit)
        //   ^body: (inlined; yields -> cf.br ^cond; breaks -> cf.br ^exit)
        //   ^exit: (everything after the loop)
        void lowerWhileLoopToCF(lf::WhileLoopOp op)
        {
            op.getBodyExec().replaceAllUsesWith(op.getIn());
            op.getDoneExec().replaceAllUsesWith(op.getIn());

            mlir::Block* preheader = op->getBlock();
            mlir::Region* parent = preheader->getParent();
            mlir::Block* exit = preheader->splitBlock(op);
            auto loc = op.getLoc();

            mlir::Region& condRegion = op.getCondRegion();
            mlir::Region& bodyRegion = op.getBodyRegion();
            mlir::Block* condEntry = &condRegion.front();
            mlir::Block* bodyEntry = &bodyRegion.front();

            condEntry->getArgument(0).replaceAllUsesWith(op.getIn());
            condEntry->eraseArgument(0);
            bodyEntry->getArgument(0).replaceAllUsesWith(op.getIn());
            bodyEntry->eraseArgument(0);

            for (lf::CondYieldOp cy : collectTerminators<lf::CondYieldOp>(condRegion))
            {
                mlir::OpBuilder b(cy);
                b.create<cf::CondBranchOp>(loc, cy.getCond(), bodyEntry, mlir::ValueRange{}, exit, mlir::ValueRange{});
                cy.erase();
            }
            for (lf::YieldOp y : collectTerminators<lf::YieldOp>(bodyRegion))
            {
                mlir::OpBuilder b(y);
                b.create<cf::BranchOp>(loc, condEntry);
                y.erase();
            }
            for (lf::BreakOp brk : collectTerminators<lf::BreakOp>(bodyRegion))
            {
                mlir::OpBuilder b(brk);
                b.create<cf::BranchOp>(loc, exit);
                brk.erase();
            }

            inlineRegionBefore(condRegion, *parent, exit);
            inlineRegionBefore(bodyRegion, *parent, exit);

            mlir::OpBuilder b(preheader, preheader->end());
            b.create<cf::BranchOp>(loc, condEntry);
            op.erase();
        }

        // Per function: append an exit block (func.return of its block args) and
        // retarget every flowforge.return to it. Returns false on a type
        // mismatch between a return's operands and the function's results.
        bool lowerReturnsToExit(mlir::func::FuncOp fn)
        {
            if (fn.getBody().empty())
                return true; // declaration

            llvm::SmallVector<lf::ReturnOp, 4> returns;
            fn.walk([&](lf::ReturnOp r) { returns.push_back(r); });
            if (returns.empty())
                return true;

            auto resultTys = fn.getFunctionType().getResults();
            mlir::Block* exit = fn.addBlock();
            llvm::SmallVector<mlir::Location, 2> locs(resultTys.size(), fn.getLoc());
            exit->addArguments(resultTys, locs);
            {
                mlir::OpBuilder b(exit, exit->end());
                b.create<mlir::func::ReturnOp>(fn.getLoc(), exit->getArguments());
            }

            for (lf::ReturnOp r : returns)
            {
                if (r.getRets().getTypes() != resultTys)
                {
                    r.emitError("return value types do not match the function's "
                                "result types");
                    return false;
                }
                mlir::OpBuilder b(r);
                b.create<cf::BranchOp>(r.getLoc(), exit, r.getRets());
                r.erase();
            }
            return true;
        }

        class FlowForgeToCFPass : public mlir::PassWrapper<FlowForgeToCFPass, mlir::OperationPass<mlir::ModuleOp>>
        {
        public:
            MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(FlowForgeToCFPass)

            llvm::StringRef getArgument() const final
            {
                return "flowforge-to-cf";
            }
            llvm::StringRef getDescription() const final
            {
                return "Lower flowforge dialect ops to cf + func + arith";
            }

            void getDependentDialects(mlir::DialectRegistry& reg) const override
            {
                reg.insert<cf::ControlFlowDialect, mlir::func::FuncDialect, mlir::arith::ArithDialect>();
            }

            void runOnOperation() override
            {
                auto module = getOperation();

                // Step 1. Control ops, innermost first.
                llvm::SmallVector<mlir::Operation*, 16> control_ops;
                module.walk<mlir::WalkOrder::PostOrder>([&](mlir::Operation* op) {
                    if (mlir::isa<lf::BranchOp, lf::ForLoopOp, lf::WhileLoopOp>(op))
                        control_ops.push_back(op);
                });
                for (auto* op : control_ops)
                {
                    if (auto br = mlir::dyn_cast<lf::BranchOp>(op))
                        lowerBranchToCF(br);
                    else if (auto fl = mlir::dyn_cast<lf::ForLoopOp>(op))
                        lowerForLoopToCF(fl);
                    else if (auto wl = mlir::dyn_cast<lf::WhileLoopOp>(op))
                        lowerWhileLoopToCF(wl);
                }

                // Step 2. token_merge: tokens are ordering artefacts; the SSA
                // value is irrelevant once they are erased.
                module.walk([](lf::TokenMergeOp op) {
                    op.getMerged().replaceAllUsesWith(op.getInputs().front());
                    op.erase();
                });

                // Step 3. returns -> per-function exit blocks.
                bool returns_ok = true;
                module.walk([&](mlir::func::FuncOp fn) { returns_ok &= lowerReturnsToExit(fn); });
                if (!returns_ok)
                    return signalPassFailure();

                // Step 4. Erase unreachable blocks FIRST (e.g. the join block of
                // a branch whose legs both returned, terminated by
                // flowforge.unreachable). Two reasons: those blocks may still
                // hold uses of the start token (which would keep flowforge.start
                // alive), and the LLVM dialect conversion framework only
                // processes REACHABLE code — dead cf ops would survive all the
                // way to LLVM IR translation and fail there with a cryptic
                // "missing translation interface" error.
                {
                    mlir::IRRewriter rewriter(module.getContext());
                    module.walk([&](mlir::func::FuncOp fn) {
                        (void)mlir::eraseUnreachableBlocks(rewriter, fn->getRegions());
                    });
                }

                // Step 4b. flowforge.start: no more consumers, erase.
                module.walk([](lf::StartOp op) {
                    if (op.getResult().use_empty())
                        op.erase();
                });

                // Step 5. Any leftover flowforge op means an unhandled shape —
                // fail explicitly so tests surface the gap.
                mlir::WalkResult leftover = module.walk([&](mlir::Operation* op) -> mlir::WalkResult {
                    if (op->getDialect() && op->getDialect()->getNamespace() == "flowforge")
                    {
                        op->emitError("FlowForge op survived lowering pass: ") << op->getName();
                        return mlir::WalkResult::interrupt();
                    }
                    return mlir::WalkResult::advance();
                });
                if (leftover.wasInterrupted())
                    signalPassFailure();
            }
        };
    } // anonymous

    static std::unique_ptr<mlir::Pass> createFlowForgeToCFPass()
    {
        return std::make_unique<FlowForgeToCFPass>();
    }

    static FlowForgeResult<void> lowerToCFImpl(IR& ir)
    {
        mlir::ModuleOp module = ir.impl().top_module.get();
        if (!module)
            return lux::cxx::unexpected(FlowForgeFailure{
                .code = EFlowForgeError::LOWERING_FAILED,
                .message = "lowerToCF: empty IR module",
            });
        mlir::PassManager pm(module.getContext());
        pm.addPass(createFlowForgeToCFPass());
        if (mlir::failed(pm.run(module)))
            return lux::cxx::unexpected(FlowForgeFailure{
                .code = EFlowForgeError::LOWERING_FAILED,
                .message = "lowerToCF: FlowForge -> CF pass failed",
            });
        return {};
    }

    static FlowForgeResult<void> lowerToLLVMImpl(IR& ir)
    {
        mlir::ModuleOp module = ir.impl().top_module.get();
        if (!module)
            return lux::cxx::unexpected(FlowForgeFailure{
                .code = EFlowForgeError::LOWERING_FAILED,
                .message = "lowerToLLVM: empty IR module",
            });
        mlir::PassManager pm(module.getContext());

        // Stage 1 — FlowForge -> CF (idempotent if already lowered: the
        // leftover-check at the end of the pass is a no-op when there's
        // nothing to do).
        pm.addPass(createFlowForgeToCFPass());

        // Stage 2 — dialect-to-LLVM lowerings. Order matters: each one only
        // converts ops of its source dialect, and ConvertFuncToLLVM rewrites
        // function signatures which the others' converted ops then plug into.
        pm.addPass(mlir::createArithToLLVMConversionPass());
        pm.addPass(mlir::createConvertControlFlowToLLVMPass());
        pm.addPass(mlir::createConvertFuncToLLVMPass());

        // Stage 3 — sweep out any builtin.unrealized_conversion_cast ops that
        // remained as bridges between the per-pass dialect rewrites (e.g. the
        // index<->i64 round-trips around the loop IV). Without this, LLVM IR
        // translation fails on unrealized_conversion_cast.
        pm.addPass(mlir::createReconcileUnrealizedCastsPass());

        if (mlir::failed(pm.run(module)))
            return lux::cxx::unexpected(FlowForgeFailure{
                .code = EFlowForgeError::LOWERING_FAILED,
                .message = "lowerToLLVM: lowering pipeline failed",
            });
        return {};
    }

    static FlowForgeResult<int> runMainJITImpl(IR& ir, const std::vector<JitNativeSymbol>& native_symbols)
    {
        mlir::ModuleOp module = ir.impl().top_module.get();
        if (!module)
            return lux::cxx::unexpected(FlowForgeFailure{
                .code = EFlowForgeError::LOWERING_FAILED,
                .message = "runMainJIT: empty IR module",
            });

        // Idempotently make sure the host LLVM target is registered. Calling
        // this from a translation unit that statically links the X86 codegen
        // libs (via the private dialect target) guarantees the X86
        // init code is actually pulled into the binary, regardless of whether
        // the caller's main() already did it.
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();

        // Make sure the LLVM dialect's MLIR -> LLVM IR translation is registered
        // on the context — ExecutionEngine needs it to emit IR for codegen.
        auto* ctx = module.getContext();
        mlir::registerBuiltinDialectTranslation(*ctx);
        mlir::registerLLVMDialectTranslation(*ctx);

        auto lowered = lowerToLLVMImpl(ir);
        if (!lowered)
            return lux::cxx::unexpected(std::move(lowered.error()));

        // O2 codegen: same LLVM backend as AOT — the default (O0) leaves easy
        // performance on the table for editor preview runs.
        mlir::ExecutionEngineOptions engine_options;
        engine_options.jitCodeGenOptLevel = llvm::CodeGenOptLevel::Default;

        auto maybeEngine = mlir::ExecutionEngine::create(module, engine_options);
        if (!maybeEngine)
            return lux::cxx::unexpected(FlowForgeFailure{
                .code = EFlowForgeError::JIT_ENGINE_CREATION_FAILED,
                .message = "runMainJIT: ExecutionEngine::create failed: " + llvm::toString(maybeEngine.takeError()),
            });
        auto& engine = *maybeEngine;

        // Bind host functions for the graph's NATIVE_FUNC_CALL extern
        // declarations — ORC does not search the host process's exports on
        // Windows, so unresolved symbols would fail at materialization.
        if (!native_symbols.empty())
        {
            engine->registerSymbols([&](llvm::orc::MangleAndInterner mangle) {
                llvm::orc::SymbolMap map;
                for (const JitNativeSymbol& sym : native_symbols)
                {
                    if (!sym.name || !sym.address)
                        continue;
                    map[mangle(sym.name)] = llvm::orc::ExecutorSymbolDef{
                        llvm::orc::ExecutorAddr::fromPtr(sym.address),
                        llvm::JITSymbolFlags::Exported
                    };
                }
                return map;
            });
        }

        auto expectedFn = engine->lookup("main");
        if (!expectedFn)
            return lux::cxx::unexpected(FlowForgeFailure{
                .code = EFlowForgeError::JIT_SYMBOL_LOOKUP_FAILED,
                .message = "runMainJIT: lookup(main) failed: " + llvm::toString(expectedFn.takeError()),
            });

        // Every generated function takes the instance-state base pointer as its
        // leading argument (graph variables live there — see StateLayout.hpp).
        // This convenience entry owns a throwaway block initialized from the
        // build-time defaults; production hosts (FlowScriptInstance / the AOT
        // loader) manage per-instance blocks themselves.
        std::vector<std::byte> state(std::max<uint64_t>(ir.impl().state_size, 1), std::byte{0});
        const auto& defaults = ir.impl().state_defaults;
        if (!defaults.empty())
            std::memcpy(state.data(), defaults.data(), std::min(defaults.size(), state.size()));

        auto* fnMain = reinterpret_cast<void (*)(void*)>(expectedFn.get());
        fnMain(state.data());
        return 0;
    }

    FlowForgeResult<void> lowerToCF(IR& ir) noexcept
    {
        try
        {
            return lowerToCFImpl(ir);
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(FlowForgeFailure{.code = EFlowForgeError::ALLOCATION_FAILURE});
        }
        catch (...)
        {
            return lux::cxx::unexpected(FlowForgeFailure{.code = EFlowForgeError::FOREIGN_EXCEPTION});
        }
    }

    FlowForgeResult<void> lowerToLLVM(IR& ir) noexcept
    {
        try
        {
            return lowerToLLVMImpl(ir);
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(FlowForgeFailure{.code = EFlowForgeError::ALLOCATION_FAILURE});
        }
        catch (...)
        {
            return lux::cxx::unexpected(FlowForgeFailure{.code = EFlowForgeError::FOREIGN_EXCEPTION});
        }
    }

    FlowForgeResult<int> runMainJIT(IR& ir) noexcept
    {
        return runMainJIT(ir, {});
    }

    FlowForgeResult<int> runMainJIT(IR& ir, const std::vector<JitNativeSymbol>& native_symbols) noexcept
    {
        try
        {
            return runMainJITImpl(ir, native_symbols);
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(FlowForgeFailure{.code = EFlowForgeError::ALLOCATION_FAILURE});
        }
        catch (...)
        {
            return lux::cxx::unexpected(FlowForgeFailure{.code = EFlowForgeError::FOREIGN_EXCEPTION});
        }
    }

} // namespace lux::flowforge
