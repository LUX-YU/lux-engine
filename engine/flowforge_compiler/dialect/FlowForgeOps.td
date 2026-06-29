//===- LuxFlow.td ------------------------------------------------*- tablegen -*-===//
// Dialect for FlowForge graphs (region-aware version).
//===----------------------------------------------------------------------===//

include "mlir/Interfaces/ControlFlowInterfaces.td"
include "mlir/Interfaces/InferTypeOpInterface.td"
include "mlir/Interfaces/SideEffectInterfaces.td"
include "mlir/IR/OpBase.td"
include "FlowForgeCompatTraits.td"

//==============================================================================
// Type definitions (unchanged)
//==============================================================================
class FlowForge_Type<string name, string mnem>
  : TypeDef<FlowForgeDialect, name> { let mnemonic = mnem; }

def TokenType     : FlowForge_Type<"token",  "token">;
def ObjectRefType : FlowForge_Type<"object", "object">;
def StringType    : FlowForge_Type<"string", "string">;

//==============================================================================
// Base op class
//==============================================================================
class FlowForge_Op<string mnem, list<Trait> traits = [Pure]>
  : Op<FlowForgeDialect, mnem, traits>;

/*-----------------------------------------------------------------------------
 *  Terminators / helpers
 *---------------------------------------------------------------------------*/
// Generic value-yield terminator reused by several region ops.
def YieldOp : FlowForge_Op<"yield", [Pure, Terminator,
    ParentOneOf<["BranchOp", "SequenceOp", "ForLoopOp", "WhileLoopOp"]>]>
{
  let summary   = "Yield values from a region";
  let arguments = (ins Variadic<AnyType>:$vals);
  // Optional operand list → must use anchor (^) so tblgen knows what to repeat.
  let assemblyFormat = [{ attr-dict ($vals^ `:` type($vals))? }];
}

// Terminator that passes next-token + condition back to WhileLoop.
def CondYieldOp : FlowForge_Op<"cond_yield",
    [Pure, Terminator, HasParent<"WhileLoopOp">]> {
  let summary = "terminator for while cond region";
  let arguments = (ins TokenType:$tok_to_body, I1:$next_cond);
  let assemblyFormat = "$tok_to_body `,` $next_cond attr-dict `:` type($tok_to_body)";
}

/*-----------------------------------------------------------------------------
 *  Region-aware Flow-graph nodes
 *---------------------------------------------------------------------------*/
// ① Start --------------------------------------------------------------------
def StartOp : FlowForge_Op<"start"> {
  let summary = "Entry node: produce initial exec token";
  let results = (outs TokenType:$exec);
}

// ② Branch -------------------------------------------------------------------
def BranchOp : FlowForge_Op<"branch",
    [DeclareOpInterfaceMethods<RegionBranchOpInterface>,
     SingleBlockImplicitTerminator<"YieldOp">]> {
  let summary   = "Structured conditional branch";
  let arguments = (ins TokenType:$in, I1:$cond);
  let results   = (outs TokenType:$trueExec, TokenType:$falseExec);
  let regions   = (region SizedRegion<1>:$thenRegion, SizedRegion<1>:$elseRegion);
}

// ③ Sequence (fan-out) -------------------------------------------------------
def SequenceOp : FlowForge_Op<"sequence",
    [DeclareOpInterfaceMethods<RegionBranchOpInterface>,
     SingleBlockImplicitTerminator<"YieldOp">]> {
  let summary   = "Fan-out execution to N successors";
  let arguments = (ins TokenType:$in);
  let results   = (outs Variadic<TokenType>:$outs);
  let regions   = (region SizedRegion<1>:$region);
}

// ④ For-loop -----------------------------------------------------------------
def ForLoopOp : FlowForge_Op<"for_loop",
    [DeclareOpInterfaceMethods<RegionBranchOpInterface>,
     SingleBlockImplicitTerminator<"YieldOp">]> {
  let summary   = "Structured for-loop node";
  let arguments = (ins TokenType:$in, Index:$first, Index:$last);
  let results   = (outs TokenType:$bodyExec, TokenType:$doneExec, Index:$iv);
  let regions   = (region SizedRegion<1>:$body);
}

// ⑤ While-loop ---------------------------------------------------------------
def WhileLoopOp : FlowForge_Op<"while_loop",
    [DeclareOpInterfaceMethods<RegionBranchOpInterface>, SingleBlock]> {
  let summary   = "Structured while-loop node";
  let arguments = (ins TokenType:$in, I1:$cond);
  let results   = (outs TokenType:$bodyExec, TokenType:$doneExec);
  let regions   = (region SizedRegion<1>:$condRegion, SizedRegion<1>:$bodyRegion);
}

// ⑥ Return -------------------------------------------------------------------
def ReturnOp : FlowForge_Op<"return", [Terminator]> {
  let summary   = "Graph final return";
  let arguments = (ins TokenType:$in, Variadic<AnyType>:$rets);
}

// ⑦ TokenMerge --------------------------------------------------------------
// Joins control tokens from convergent legs (e.g. both arms of a Branch
// reaching the same downstream node). Exactly one input is live at runtime;
// the result is "the surviving token". This is the dialect-level placeholder
// for the LLVM-PHI that the FlowForge -> LLVM lowering pass will materialize
// in the join basic block. No runtime cost.
def TokenMergeOp : FlowForge_Op<"token_merge"> {
  let summary   = "Merge convergent control tokens into one";
  let arguments = (ins Variadic<TokenType>:$inputs);
  let results   = (outs TokenType:$merged);
  let assemblyFormat = [{ attr-dict ($inputs^ `:` type($inputs))? }];
}
