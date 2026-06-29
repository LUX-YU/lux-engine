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
        addTypes<
            FLOWFORGE_TOKEN_TYPE,
            FLOWFORGE_OBJECT_TYPE,
            FLOWFORGE_STRING_TYPE
        >();

        addOperations<
            StartOp,
			WhileLoopOp,
			ForLoopOp,
			BranchOp,
			SequenceOp,
            YieldOp,
            CondYieldOp,
            ReturnOp,
            TokenMergeOp
		>();
    }


    FlowForgeDialect::~FlowForgeDialect() = default;

    // -------------------------------------------------------------------------
    // BranchOp
    // -------------------------------------------------------------------------
    void BranchOp::getSuccessorRegions(FLOWFORGE_REGION_BRANCH_PARAM,
        SmallVectorImpl<RegionSuccessor>& regions) 
    {
        if (FLOWFORGE_IS_PARENT_REGION) {
            // Coming from parent operation
            regions.emplace_back(&FLOWFORGE_GET_THEN_REGION(*this), FLOWFORGE_GET_THEN_REGION(*this).getArguments());
            regions.emplace_back(&FLOWFORGE_GET_ELSE_REGION(*this), FLOWFORGE_GET_ELSE_REGION(*this).getArguments());
            return;
        }

        // Coming from one of the child regions - determine which one
        Region* fromRegion = FLOWFORGE_GET_REGION_OR_NULL;
        if (fromRegion == &FLOWFORGE_GET_THEN_REGION(*this)) {
            // From then region -> trueExec (result 0)
            regions.emplace_back(getResults().slice(0, 1));
        } else {
            // From else region -> falseExec (result 1)
            regions.emplace_back(getResults().slice(1, 1));
        }
    }

    // ---------------- SequenceOp --------------
    void SequenceOp::getSuccessorRegions(FLOWFORGE_REGION_BRANCH_PARAM, SmallVectorImpl<RegionSuccessor>& regions) 
    {
        if (FLOWFORGE_IS_PARENT_REGION) {
            // Coming from parent operation
            regions.emplace_back(&getRegion(), getRegion().getArguments());
        }
        else {
            // Coming from child region
            regions.emplace_back(getResults()); // 全部 fan-out token
        }
    }

    // ---------------- ForLoopOp ---------------
    void ForLoopOp::getSuccessorRegions(FLOWFORGE_REGION_BRANCH_PARAM,
        SmallVectorImpl<RegionSuccessor>& regions) {
        if (FLOWFORGE_IS_PARENT_REGION)
        {
            // Coming from parent operation
            regions.emplace_back(&FLOWFORGE_GET_BODY_REGION(*this), FLOWFORGE_GET_BODY_REGION(*this).getArguments());
            return;
        }

        // From body region - can loop back to body or exit
        regions.emplace_back(&FLOWFORGE_GET_BODY_REGION(*this), FLOWFORGE_GET_BODY_REGION(*this).getArguments());
        regions.emplace_back(getResults().slice(1, 1)); // doneExec
    }

    // ---------------- WhileLoopOp -------------
    void WhileLoopOp::getSuccessorRegions(FLOWFORGE_REGION_BRANCH_PARAM,
        SmallVectorImpl<RegionSuccessor>& regions) {
        if (FLOWFORGE_IS_PARENT_REGION)
        {
            // Coming from parent operation
            regions.emplace_back(&FLOWFORGE_GET_COND_REGION(*this),
                FLOWFORGE_GET_COND_REGION(*this).getArguments());
            return;
        }

        // Determine which region we're coming from
        Region* fromRegion = FLOWFORGE_GET_REGION_OR_NULL;
        if (fromRegion == &FLOWFORGE_GET_COND_REGION(*this)) {
            // From cond region -> can go to body or done
            regions.emplace_back(&FLOWFORGE_GET_BODY_REGION_WHILE(*this), FLOWFORGE_GET_BODY_REGION_WHILE(*this).getArguments());
            regions.emplace_back(getResults().slice(1, 1)); // doneExec is result 1
        } else {
            // From body region -> back to cond
            regions.emplace_back(&FLOWFORGE_GET_COND_REGION(*this), FLOWFORGE_GET_COND_REGION(*this).getArguments());
        }
    }
}
