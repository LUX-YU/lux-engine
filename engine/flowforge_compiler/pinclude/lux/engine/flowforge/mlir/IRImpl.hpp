#pragma once
#include <lux/engine/flowforge/mlir/IR.hpp>

#include <mlir/IR/MLIRContext.h>
#include <mlir/IR/BuiltinOps.h>
#include <mlir/IR/Builders.h>
#include <mlir/IR/BuiltinTypes.h>
#include <mlir/IR/BuiltinAttributes.h>
#include <mlir/Dialect/Func/IR/FuncOps.h>

// Include version-compatible headers
#include "FlowForgeVersionCompat.h"
#include FLOWFORGE_ARITH_DIALECT_INCLUDE

namespace lux::flowforge
{
    class IRImpl
    {
    public:
        mlir::ModuleOp top_module;
    };
}
