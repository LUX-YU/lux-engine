//===- LuxFlow.td ------------------------------------------------*- tablegen -*-===//
//  Minimal but build‑ready MLIR dialect for FlowForge graphs.
//  Save as LuxFlow.td and add to your CMakeLists with add_mlir_dialect().
//  Tested against LLVM/MLIR main (2025‑04‑23).
//===----------------------------------------------------------------------===//
include "mlir/Interfaces/SideEffectInterfaces.td"
include "mlir/Interfaces/ControlFlowInterfaces.td"
include "mlir/IR/BuiltinTypes.td"

//------------------------------------------------------------------------------
// Dialect declaration
//------------------------------------------------------------------------------
def FlowForgeDialect : Dialect {
  let name          = "flowforge"; // appears as the textual prefix
  let summary       = "High‑level FlowForge graph dialect";
  let cppNamespace  = "::mlir::flowforge";    // C++ namespace for generated classes

  // Extra class declarations
  // let extraClassDeclarations = [{
  //   static void sayhello();
  // }];
  let dependentDialects = ["::mlir::tensor::TensorDialect"];

  // Constant-materializer bit
  let hasConstantMaterializer = 0;

  // Whether to generate the default destructor bit
  let hasNonDefaultDestructor = 1;

  // Operand attribute verification
  // let hasOperationAtrrVerify = 0;

  // Whether to generate the operand attribute verifier bit
  // let hasRegionArgAttrVerify = 0;

  // Region result attribute verifier bit
  // let hasRegionResultAttrVerify = 0;

  // Whether to generate the operand attribute verifier bit
  // let hasOperationInterfaceFallback = 0;

  // Whether to use MLIR's default attribute printer bit
  let useDefaultAttributePrinterParser = 0;

  //
  // let useDefaultTypePrinterParser = 0;

  // Whether there is a canonicalizer bit
  // let hasCanonicalizer = 0;

  // let isExtensible = 0;

  // let usePropertiesForAttributes = 0;

  let useDefaultTypePrinterParser = 1;
}
