#pragma once

#include <lux/engine/flowforge/compiler/IR.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

#include <mlir/IR/MLIRContext.h>
#include <mlir/IR/BuiltinOps.h>
#include <mlir/IR/Builders.h>
#include <mlir/IR/BuiltinTypes.h>
#include <mlir/IR/BuiltinAttributes.h>
#include <mlir/IR/OwningOpRef.h>
#include <mlir/Dialect/Func/IR/FuncOps.h>

// Include version-compatible headers
#include "FlowForgeVersionCompat.h"
#include FLOWFORGE_ARITH_DIALECT_INCLUDE

namespace lux::flowforge
{
    class IRImpl
    {
    public:
        // Owning: generated modules are detached ops, so the ref's
        // destructor is what reclaims them (a plain ModuleOp handle would
        // leak one module per generateIR call). Must not outlive the
        // IRContext whose MLIRContext allocated the module.
        mlir::OwningOpRef<mlir::ModuleOp> top_module;

        // Instance-state recipe captured at build time (see StateLayout.hpp):
        // graph variables live in a HOST-owned block whose base pointer is
        // every generated function's leading argument. Hosts (runMainJIT /
        // FlowScriptInstance / the AOT loader) allocate `state_size` bytes
        // and copy `state_defaults` in before the first invoke. Kept on the
        // IR object rather than as module attributes so the MLIR -> LLVM
        // translation never sees foreign dialect attributes.
        uint64_t               state_size = 0;
        uint64_t               state_hash = 0;
        uint32_t               state_align = 1;
        std::vector<std::byte> state_defaults;
    };
}
