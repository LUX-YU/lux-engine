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

  // 额外的类声明
  // let extraClassDeclarations = [{
  //   static void sayhello();
  // }];
  let dependentDialects = ["::mlir::tensor::TensorDialect"];

  // 规范化的声明bit
  let hasConstantMaterializer = 0;

  // 是否生成默认的析构函数bit
  let hasNonDefaultDestructor = 1;

  // 操作数的属性检验
  // let hasOperationAtrrVerify = 0;

  // 是否生成操作数的属性验证器bit
  // let hasRegionArgAttrVerify = 0;

  // Region Result的属性验证器bit
  // let hasRegionResultAttrVerify = 0;

  // 是否生成操作数的属性验证器bit
  // let hasOperationInterfaceFallback = 0;

  // 使用MLIR的默认属性打印器bit
  let useDefaultAttributePrinterParser = 0;

  // 
  // let useDefaultTypePrinterParser = 0;

  // 是否有规范化器bit
  // let hasCanonicalizer = 0;

  // let isExtensible = 0;

  // let usePropertiesForAttributes = 0;

  let useDefaultTypePrinterParser = 1;
}
