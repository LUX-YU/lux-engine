#include "FlowForgeDialect.h"
#include "FlowForgeVersionCompat.h"
#include <mlir/IR/Builders.h>
#include <mlir/IR/BuiltinOps.h>
#include <mlir/Support/LLVM.h>
#include <mlir/Interfaces/ControlFlowInterfaces.h>
#include <llvm/ADT/TypeSwitch.h>
#include "llvm/Support/raw_ostream.h"
#include "FlowForgeDialect.cpp.inc"

#define GET_OP_CLASSES
#include "FlowForgeOps.cpp.inc"
#undef GET_OP_CLASSES
#define GET_TYPEDEF_CLASSES
#include "FlowForgeTypes.cpp.inc"
#undef GET_TYPEDEF_CLASSES

namespace mlir::flowforge
{
    void FlowForgeDialect::initialize()
    {
        addTypes<FLOWFORGE_TOKEN_TYPE, FLOWFORGE_OBJECT_TYPE, FLOWFORGE_STRING_TYPE>();

        addOperations<
            StartOp,
            WhileLoopOp,
            ForLoopOp,
            BranchOp,
            YieldOp,
            CondYieldOp,
            BreakOp,
            UnreachableOp,
            ReturnOp,
            TokenMergeOp>();
    }

    FlowForgeDialect::~FlowForgeDialect() = default;

    // -------------------------------------------------------------------------
    // BranchOp
    //
    // Control-flow contract: the parent enters exactly one of then/else; each
    // region's yield maps to that leg's result token. The $in token operand
    // is forwarded to the entered region's block-arg.
    // -------------------------------------------------------------------------
    OperandRange BranchOp::getEntrySuccessorOperands(RegionBranchPoint)
    {
        // $in -> the leg's token block-arg (same for both regions).
        return getOperands().slice(0, 1);
    }

    void BranchOp::getSuccessorRegions(FLOWFORGE_REGION_BRANCH_PARAM, SmallVectorImpl<RegionSuccessor>& regions)
    {
        if (FLOWFORGE_IS_PARENT_REGION)
        {
            // Coming from parent operation: exactly one leg runs.
            regions.emplace_back(&FLOWFORGE_GET_THEN_REGION(*this), FLOWFORGE_GET_THEN_REGION(*this).getArguments());
            regions.emplace_back(&FLOWFORGE_GET_ELSE_REGION(*this), FLOWFORGE_GET_ELSE_REGION(*this).getArguments());
            return;
        }

        // Coming from one of the child regions - determine which one
        Region* fromRegion = FLOWFORGE_GET_REGION_OR_NULL;
        if (fromRegion == &FLOWFORGE_GET_THEN_REGION(*this))
        {
            // From then region -> trueExec (result 0)
            regions.emplace_back(getResults().slice(0, 1));
        }
        else
        {
            // From else region -> falseExec (result 1)
            regions.emplace_back(getResults().slice(1, 1));
        }
    }

    // ---------------- ForLoopOp ---------------
    //
    // Control-flow contract mirrors scf.for: the induction variable is a
    // loop-defined block-arg and is NOT part of any control-flow edge; only
    // the token (block-arg 0) flows along edges. From the parent, control
    // either enters the body or (zero-trip) goes straight to the results.
    OperandRange ForLoopOp::getEntrySuccessorOperands(RegionBranchPoint)
    {
        // $in -> body token block-arg / doneExec result (zero-trip).
        return getOperands().slice(0, 1);
    }

    void ForLoopOp::getSuccessorRegions(FLOWFORGE_REGION_BRANCH_PARAM, SmallVectorImpl<RegionSuccessor>& regions)
    {
        auto& body = FLOWFORGE_GET_BODY_REGION(*this);
        // Token block-arg only — the IV (block-arg 1) is loop-defined.
        auto bodyTokenArg = body.getArguments().slice(0, 1);
        if (FLOWFORGE_IS_PARENT_REGION)
        {
            regions.emplace_back(&body, bodyTokenArg);
            regions.emplace_back(getResults().slice(1, 1)); // zero-trip -> doneExec
            return;
        }

        // From body region - can loop back to body or exit
        regions.emplace_back(&body, bodyTokenArg);
        regions.emplace_back(getResults().slice(1, 1)); // doneExec
    }

    // ---------------- WhileLoopOp -------------
    //
    // Control-flow contract: parent enters the cond region; the cond region
    // either enters the body (cond true) or exits to the results (cond
    // false); the body loops back to the cond region.
    OperandRange WhileLoopOp::getEntrySuccessorOperands(RegionBranchPoint)
    {
        // $in -> cond region's token block-arg. (Single-operand ops only get
        // the singular getOperand() accessor, hence the getOperation() form.)
        return getOperation()->getOperands().slice(0, 1);
    }

    void WhileLoopOp::getSuccessorRegions(FLOWFORGE_REGION_BRANCH_PARAM, SmallVectorImpl<RegionSuccessor>& regions)
    {
        auto& cond = getCondRegion();
        auto& body = getBodyRegion();
        if (FLOWFORGE_IS_PARENT_REGION)
        {
            regions.emplace_back(&cond, cond.getArguments());
            return;
        }

        Region* fromRegion = FLOWFORGE_GET_REGION_OR_NULL;
        if (fromRegion == &cond)
        {
            // cond true -> body; cond false -> doneExec
            regions.emplace_back(&body, body.getArguments());
            regions.emplace_back(getResults().slice(1, 1));
        }
        else
        {
            // body -> back to cond
            regions.emplace_back(&cond, cond.getArguments());
        }
    }

    // ---------------- CondYieldOp -------------
    //
    // Only the token is a forwarded control-flow-edge operand; $cond is the
    // branch decision itself.
    MutableOperandRange CondYieldOp::getMutableSuccessorOperands(RegionBranchPoint)
    {
        return MutableOperandRange(getOperation(), /*start=*/0, /*length=*/1);
    }
}
