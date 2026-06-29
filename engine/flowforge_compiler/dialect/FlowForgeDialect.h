#pragma once
#ifndef FLOWFORGE_DIALECT_FLOWFORGEDIALECT_H
#define FLOWFORGE_DIALECT_FLOWFORGEDIALECT_H
#include <mlir/IR/DialectImplementation.h>
#include <mlir/IR/Dialect.h>
#include <mlir/Dialect/Tensor/IR/Tensor.h>
#include <mlir/IR/BuiltinTypes.h>
#include <mlir/IR/MLIRContext.h>
#include "FlowForgeDialect.h.inc"

#define GET_TYPEDEF_CLASSES
#include "FlowForgeTypes.h.inc"
#undef GET_TYPEDEF_CLASSES

#define GET_OP_CLASSES
#include "FlowForgeOps.h.inc"
#undef GET_OP_CLASSES
#endif

